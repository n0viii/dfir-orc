//
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright © 2011-2019 ANSSI. All Rights Reserved.
//
// Author(s): Jean Gautier (ANSSI)
//

#include "stdafx.h"

#include "GetThis.h"

#include <string>
#include <memory>
#include <filesystem>
#include <sstream>

#include "TableOutput.h"
#include "CsvFileWriter.h"
#include "ConfigFileReader.h"
#include "FileFind.h"
#include "ByteStream.h"
#include "FileStream.h"
#include "MemoryStream.h"
#include "TemporaryStream.h"
#include "DevNullStream.h"
#include "StringsStream.h"
#include "CryptoHashStream.h"
#include "ParameterCheck.h"
#include "ArchiveExtract.h"

#include "SnapshotVolumeReader.h"

#include "SystemDetails.h"
#include "WinApiHelper.h"

#include "NtfsDataStructures.h"

using namespace std;
namespace fs = std::filesystem;

using namespace Orc;
using namespace Orc::Command::GetThis;

namespace {

enum class CompressorFlags : uint32_t
{
    kNone = 0,
    kComputeHash = 1
};

std::shared_ptr<ArchiveCreate>
CreateCompressor(const OutputSpec& outputSpec, CompressorFlags flags, HRESULT& hr, logger& _L_)
{
    const bool computeHash = (static_cast<uint32_t>(flags) & static_cast<uint32_t>(CompressorFlags::kComputeHash));

    auto compressor = ArchiveCreate::MakeCreate(outputSpec.ArchiveFormat, _L_, computeHash);
    if (compressor == nullptr)
    {
        hr = E_POINTER;
        log::Error(_L_, hr, L"Failed calling MakeCreate for archive '%s'\r\n", outputSpec.Path.c_str());
        return nullptr;
    }

    hr = compressor->InitArchive(outputSpec.Path);
    if (FAILED(hr))
    {
        log::Error(_L_, hr, L"Failed to initialize archive '%s'\r\n", outputSpec.Path.c_str());
        return nullptr;
    }

    if (!outputSpec.Password.empty())
    {
        hr = compressor->SetPassword(outputSpec.Password);
        if (FAILED(hr))
        {
            log::Error(_L_, hr, L"Failed to set password for '%s'\r\n", outputSpec.Path.c_str());
            return nullptr;
        }
    }

    hr = compressor->SetCompressionLevel(outputSpec.Compression);
    if (FAILED(hr))
    {
        log::Error(_L_, hr, L"Failed to set compression level for '%s'\r\n", outputSpec.Path.c_str());
        return nullptr;
    }

    compressor->SetCallback(
        [&_L_](const OrcArchive::ArchiveItem& item) { log::Info(_L_, L"\t%s\r\n", item.Path.c_str()); });

    return compressor;
}

std::shared_ptr<TableOutput::IStreamWriter> CreateCsvWriter(
    const std::filesystem::path& out,
    const Orc::TableOutput::Schema& schema,
    const OutputSpec::Encoding& encoding,
    HRESULT& hr)
{
    auto csvStream = std::make_shared<TemporaryStream>();

    hr = csvStream->Open(out.parent_path(), out.filename(), 5 * 1024 * 1024);
    if (FAILED(hr))
    {
        Log::Error(L"Failed to create temp stream (code: {:#x})", hr);
        return nullptr;
    }

    auto options = std::make_unique<TableOutput::CSV::Options>();
    options->Encoding = encoding;

    auto csvWriter = TableOutput::CSV::Writer::MakeNew(std::move(options));
    hr = csvWriter->WriteToStream(csvStream);
    if (FAILED(hr))
    {
        Log::Error(L"Failed to initialize CSV stream (code: {:#x})", hr);
        return nullptr;
    }

    hr = csvWriter->SetSchema(schema);
    if (FAILED(hr))
    {
        Log::Error(L"Failed to set CSV schema (code: {:#x})", hr);
        return nullptr;
    }

    return csvWriter;
}

std::shared_ptr<TemporaryStream> CreateLogStream(const std::filesystem::path& out, HRESULT& hr, logger& _L_)
{
    auto logWriter = std::make_shared<LogFileWriter>(0x1000);
    logWriter->SetConsoleLog(_L_->ConsoleLog());
    logWriter->SetDebugLog(_L_->DebugLog());
    logWriter->SetVerboseLog(_L_->VerboseLog());

    auto logStream = std::make_shared<TemporaryStream>(logWriter);

    hr = logStream->Open(out.parent_path(), out.filename(), 5 * 1024 * 1024);
    if (FAILED(hr))
    {
        log::Error(_L_, hr, L"Failed to create temp stream\r\n");
        return nullptr;
    }

    hr = _L_->LogToStream(logStream);
    if (FAILED(hr))
    {
        log::Error(_L_, hr, L"Failed to initialize temp logging\r\n");
        return nullptr;
    }

    return logStream;
}

std::wstring RetrieveComputerName(const std::wstring& defaultName, logger& _L_)
{
    std::wstring name;

    HRESULT hr = SystemDetails::GetOrcComputerName(name);
    if (FAILED(hr))
    {
        Log::Error(L"Failed to retrieve computer name (code: {:#x})", hr);
        return L"[unknown]";
    }

    return name;
}

HRESULT CopyStream(Orc::ByteStream& src, const fs::path& outPath)
{
    HRESULT hr = E_FAIL;
    std::error_code ec;

    fs::create_directories(outPath.parent_path(), ec);
    if (ec)
    {
        hr = HRESULT_FROM_WIN32(ec.value());
        Log::Error(L"Failed to create sample directory (code: {:#x})", hr);
        return hr;
    }

    FileStream outputStream;
    hr = outputStream.WriteTo(outPath.c_str());
    if (FAILED(hr))
    {
        Log::Error(L"Failed to create sample '{}' (code: {:#x})", outPath);
        return hr;
    }

    ULONGLONG ullBytesWritten = 0LL;
    hr = src.CopyTo(outputStream, &ullBytesWritten);
    if (FAILED(hr))
    {
        Log::Error(L"Failed while writing sample '{}' (code: {:#x})", outPath, hr);
        return hr;
    }

    hr = outputStream.Close();
    if (FAILED(hr))
    {
        Log::Error(L"Failed to close sample '{}' (code: {:#x})", outPath, hr);
        return hr;
    }

    hr = src.Close();
    if (FAILED(hr))
    {
        Log::Warn(L"Failed to close input steam for '{}' (code: {:#x})", outPath, hr);
    }

    return S_OK;
}

}  // namespace

Main::Main(logger pLog)
    : UtilitiesMain(pLog)
    , config(pLog)
    , FileFinder(pLog)
    , ComputerName(::RetrieveComputerName(L"Default", pLog))
{
}

HRESULT Main::RegFlushKeys()
{
    bool bSuccess = true;
    DWORD dwGLE = 0L;

    log::Info(_L_, L"\r\nFlushing HKEY_LOCAL_MACHINE\r\n");
    dwGLE = RegFlushKey(HKEY_LOCAL_MACHINE);
    if (dwGLE != ERROR_SUCCESS)
        bSuccess = false;

    log::Info(_L_, L"Flushing HKEY_USERS\r\n");
    dwGLE = RegFlushKey(HKEY_USERS);
    if (dwGLE != ERROR_SUCCESS)
        bSuccess = false;

    if (!bSuccess)
        return HRESULT_FROM_WIN32(dwGLE);
    return S_OK;
}

HRESULT Main::CreateSampleFileName(
    const ContentSpec& content,
    const PFILE_NAME pFileName,
    const wstring& DataName,
    DWORD idx,
    wstring& SampleFileName)
{
    if (pFileName == NULL)
        return E_POINTER;

    WCHAR tmpName[MAX_PATH];
    WCHAR* pContent = NULL;

    switch (content.Type)
    {
        case ContentType::DATA:
            pContent = L"data";
            break;
        case ContentType::STRINGS:
            pContent = L"strings";
            break;
        case ContentType::RAW:
            pContent = L"raw";
            break;
        default:
            pContent = L"";
            break;
    }

    if (idx)
    {
        if (DataName.size())
            swprintf_s(
                tmpName,
                MAX_PATH,
                L"%*.*X%*.*X%*.*X_%.*s_%.*s_%u_%s",
                (int)sizeof(pFileName->ParentDirectory.SequenceNumber) * 2,
                (int)sizeof(pFileName->ParentDirectory.SequenceNumber) * 2,
                pFileName->ParentDirectory.SequenceNumber,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberHighPart) * 2,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberHighPart) * 2,
                pFileName->ParentDirectory.SegmentNumberHighPart,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberLowPart) * 2,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberLowPart) * 2,
                pFileName->ParentDirectory.SegmentNumberLowPart,
                pFileName->FileNameLength,
                pFileName->FileName,
                (UINT)DataName.size(),
                DataName.c_str(),
                idx,
                pContent);
        else
            swprintf_s(
                tmpName,
                MAX_PATH,
                L"%*.*X%*.*X%*.*X__%.*s_%u_%s",
                (int)sizeof(pFileName->ParentDirectory.SequenceNumber) * 2,
                (int)sizeof(pFileName->ParentDirectory.SequenceNumber) * 2,
                pFileName->ParentDirectory.SequenceNumber,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberHighPart) * 2,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberHighPart) * 2,
                pFileName->ParentDirectory.SegmentNumberHighPart,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberLowPart) * 2,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberLowPart) * 2,
                pFileName->ParentDirectory.SegmentNumberLowPart,
                pFileName->FileNameLength,
                pFileName->FileName,
                idx,
                pContent);
    }
    else
    {
        if (DataName.size())
            swprintf_s(
                tmpName,
                MAX_PATH,
                L"%*.*X%*.*X%*.*X__%.*s_%.*s_%s",
                (int)sizeof(pFileName->ParentDirectory.SequenceNumber) * 2,
                (int)sizeof(pFileName->ParentDirectory.SequenceNumber) * 2,
                pFileName->ParentDirectory.SequenceNumber,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberHighPart) * 2,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberHighPart) * 2,
                pFileName->ParentDirectory.SegmentNumberHighPart,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberLowPart) * 2,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberLowPart) * 2,
                pFileName->ParentDirectory.SegmentNumberLowPart,
                pFileName->FileNameLength,
                pFileName->FileName,
                (UINT)DataName.size(),
                DataName.c_str(),
                pContent);
        else
        {
            swprintf_s(
                tmpName,
                MAX_PATH,
                L"%*.*X%*.*X%*.*X_%.*s_%s",
                (int)sizeof(pFileName->ParentDirectory.SequenceNumber) * 2,
                (int)sizeof(pFileName->ParentDirectory.SequenceNumber) * 2,
                pFileName->ParentDirectory.SequenceNumber,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberHighPart) * 2,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberHighPart) * 2,
                pFileName->ParentDirectory.SegmentNumberHighPart,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberLowPart) * 2,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberLowPart) * 2,
                pFileName->ParentDirectory.SegmentNumberLowPart,
                pFileName->FileNameLength,
                pFileName->FileName,
                pContent);
        }
    }

    SampleFileName.assign(tmpName);

    std::for_each(SampleFileName.begin(), SampleFileName.end(), [=](WCHAR& Item) {
        if (iswspace(Item) || Item == L':' || Item == L'#')
            Item = L'_';
    });

    return S_OK;
}

HRESULT Main::ConfigureSampleStreams(SampleRef& sampleRef)
{
    HRESULT hr = E_FAIL;

    shared_ptr<ByteStream> retval;

    _ASSERT(sampleRef.Matches.front()->MatchingAttributes[sampleRef.AttributeIndex].DataStream->IsOpen() == S_OK);

    if (sampleRef.SampleName.empty())
        return E_INVALIDARG;

    std::shared_ptr<ByteStream> stream;

    switch (sampleRef.Content.Type)
    {
        case ContentType::DATA:
            stream = sampleRef.Matches.front()->MatchingAttributes[sampleRef.AttributeIndex].DataStream;
            break;
        case ContentType::STRINGS: {
            auto strings = std::make_shared<StringsStream>(_L_);
            if (sampleRef.Content.MaxChars == 0 && sampleRef.Content.MinChars == 0)
            {
                if (FAILED(
                        hr = strings->OpenForStrings(
                            sampleRef.Matches.front()->MatchingAttributes[sampleRef.AttributeIndex].DataStream,
                            config.content.MinChars,
                            config.content.MaxChars)))
                {
                    log::Error(_L_, hr, L"Failed to initialise strings stream\r\n");
                    return hr;
                }
            }
            else
            {
                if (FAILED(
                        hr = strings->OpenForStrings(
                            sampleRef.Matches.front()->MatchingAttributes[sampleRef.AttributeIndex].DataStream,
                            sampleRef.Content.MinChars,
                            sampleRef.Content.MaxChars)))
                {
                    log::Error(_L_, hr, L"Failed to initialise strings stream\r\n");
                    return hr;
                }
            }
            stream = strings;
        }
        break;
        case ContentType::RAW:
            stream = sampleRef.Matches.front()->MatchingAttributes[sampleRef.AttributeIndex].RawStream;
            break;
        default:
            stream = sampleRef.Matches.front()->MatchingAttributes[sampleRef.AttributeIndex].DataStream;
            break;
    }

    std::shared_ptr<ByteStream> upstream = stream;

    CryptoHashStream::Algorithm algs = config.CryptoHashAlgs;

    if (algs != CryptoHashStream::Algorithm::Undefined)
    {
        sampleRef.HashStream = make_shared<CryptoHashStream>(_L_);
        if (FAILED(hr = sampleRef.HashStream->OpenToRead(algs, upstream)))
            return hr;
        upstream = sampleRef.HashStream;
    }
    else
    {
        upstream = stream;
    }

    FuzzyHashStream::Algorithm fuzzy_algs = config.FuzzyHashAlgs;
    if (fuzzy_algs != FuzzyHashStream::Algorithm::Undefined)
    {
        sampleRef.FuzzyHashStream = make_shared<FuzzyHashStream>(_L_);
        if (FAILED(hr = sampleRef.FuzzyHashStream->OpenToRead(fuzzy_algs, upstream)))
            return hr;
        upstream = sampleRef.FuzzyHashStream;
    }

    sampleRef.CopyStream = upstream;
    sampleRef.SampleSize = sampleRef.CopyStream->GetSize();
    return S_OK;
}

LimitStatus Main::SampleLimitStatus(const Limits& GlobalLimits, const Limits& LocalLimits, DWORDLONG DataSize)
{
    if (GlobalLimits.bIgnoreLimits)
        return NoLimits;

    // Sample count reached?

    if (GlobalLimits.dwMaxSampleCount != INFINITE)
        if (GlobalLimits.dwAccumulatedSampleCount >= GlobalLimits.dwMaxSampleCount)
            return GlobalSampleCountLimitReached;

    if (LocalLimits.dwMaxSampleCount != INFINITE)
        if (LocalLimits.dwAccumulatedSampleCount >= LocalLimits.dwMaxSampleCount)
            return LocalSampleCountLimitReached;

    //  Global limits
    if (GlobalLimits.dwlMaxBytesPerSample != INFINITE)
        if (DataSize > GlobalLimits.dwlMaxBytesPerSample)
            return GlobalMaxBytesPerSample;

    if (GlobalLimits.dwlMaxBytesTotal != INFINITE)
        if (DataSize + GlobalLimits.dwlAccumulatedBytesTotal > GlobalLimits.dwlMaxBytesTotal)
            return GlobalMaxBytesTotal;

    // Local limits  bytes are now collected?
    if (LocalLimits.dwlMaxBytesPerSample != INFINITE)
        if (DataSize > LocalLimits.dwlMaxBytesPerSample)
            return LocalMaxBytesPerSample;
    if (LocalLimits.dwlMaxBytesTotal != INFINITE)
        if (DataSize + LocalLimits.dwlAccumulatedBytesTotal > LocalLimits.dwlMaxBytesTotal)
            return LocalMaxBytesTotal;

    return SampleWithinLimits;
}

HRESULT
Main::AddSampleRefToCSV(ITableOutput& output, const Main::SampleRef& sample) const
{
    static const FlagsDefinition AttrTypeDefs[] = {
        {$UNUSED, L"$UNUSED", L"$UNUSED"},
        {$STANDARD_INFORMATION, L"$STANDARD_INFORMATION", L"$STANDARD_INFORMATION"},
        {$ATTRIBUTE_LIST, L"$ATTRIBUTE_LIST", L"$ATTRIBUTE_LIST"},
        {$FILE_NAME, L"$FILE_NAME", L"$FILE_NAME"},
        {$OBJECT_ID, L"$OBJECT_ID", L"$OBJECT_ID"},
        {$SECURITY_DESCRIPTOR, L"$SECURITY_DESCRIPTOR", L"$SECURITY_DESCRIPTOR"},
        {$VOLUME_NAME, L"$VOLUME_NAME", L"$VOLUME_NAME"},
        {$VOLUME_INFORMATION, L"$VOLUME_INFORMATION", L"$VOLUME_INFORMATION"},
        {$DATA, L"$DATA", L"$DATA"},
        {$INDEX_ROOT, L"$INDEX_ROOT", L"$INDEX_ROOT"},
        {$INDEX_ALLOCATION, L"$INDEX_ALLOCATION", L"$INDEX_ALLOCATION"},
        {$BITMAP, L"$BITMAP", L"$BITMAP"},
        {$REPARSE_POINT, L"$REPARSE_POINT", L"$REPARSE_POINT"},
        {$EA_INFORMATION, L"$EA_INFORMATION", L"$EA_INFORMATION"},
        {$EA, L"$EA", L"$EA"},
        {$LOGGED_UTILITY_STREAM, L"$LOGGED_UTILITY_STREAM", L"$LOGGED_UTILITY_STREAM"},
        {$FIRST_USER_DEFINED_ATTRIBUTE, L"$FIRST_USER_DEFINED_ATTRIBUTE", L"$FIRST_USER_DEFINED_ATTRIBUTE"},
        {$END, L"$END", L"$END"}};

    for (const auto& match : sample.Matches)
    {
        for (const auto& name : match->MatchingNames)
        {
            output.WriteString(ComputerName);

            output.WriteInteger(match->VolumeReader->VolumeSerialNumber());

            {
                LARGE_INTEGER* pLI = (LARGE_INTEGER*)&(name.FILENAME()->ParentDirectory);
                output.WriteInteger((DWORDLONG)pLI->QuadPart);
            }
            {
                LARGE_INTEGER* pLI = (LARGE_INTEGER*)&(match->FRN);
                output.WriteInteger((DWORDLONG)pLI->QuadPart);
            }

            output.WriteString(name.FullPathName);

            if (sample.OffLimits)
            {
                output.WriteNothing();
            }
            else
            {
                output.WriteString(sample.SampleName);
            }

            output.WriteFileSize(sample.SampleSize);

            output.WriteBytes(sample.MD5);

            output.WriteBytes(sample.SHA1);

            output.WriteString(match->Term->GetDescription());

            switch (sample.Content.Type)
            {
                case ContentType::DATA:
                    output.WriteString(L"data");
                    break;
                case ContentType::STRINGS:
                    output.WriteString(L"strings");
                    break;
                default:
                    output.WriteNothing();
            }

            output.WriteFileTime(sample.CollectionDate);

            output.WriteFileTime(match->StandardInformation->CreationTime);
            output.WriteFileTime(match->StandardInformation->LastModificationTime);
            output.WriteFileTime(match->StandardInformation->LastAccessTime);
            output.WriteFileTime(match->StandardInformation->LastChangeTime);

            output.WriteFileTime(name.FILENAME()->Info.CreationTime);
            output.WriteFileTime(name.FILENAME()->Info.LastModificationTime);
            output.WriteFileTime(name.FILENAME()->Info.LastAccessTime);
            output.WriteFileTime(name.FILENAME()->Info.LastChangeTime);

            output.WriteExactFlags(match->MatchingAttributes[sample.AttributeIndex].Type, AttrTypeDefs);

            output.WriteString(match->MatchingAttributes[sample.AttributeIndex].AttrName);

            output.WriteInteger((DWORD)sample.InstanceID);

            output.WriteGUID(sample.SnapshotID);

            output.WriteBytes(sample.SHA256);

            output.WriteBytes(sample.SSDeep);

            output.WriteBytes(sample.TLSH);

            const auto& rules = match->MatchingAttributes[sample.AttributeIndex].YaraRules;
            if (rules.has_value())
            {
                std::stringstream aStream;
                const char* const delim = "; ";

                std::copy(
                    std::cbegin(rules.value()),
                    std::cend(rules.value()),
                    std::ostream_iterator<std::string>(aStream, delim));

                output.WriteString(aStream.str());
            }
            else
            {
                output.WriteNothing();
            }

            output.WriteEndOfLine();
        }
    }

    return S_OK;
}

HRESULT
Main::AddSamplesForMatch(LimitStatus status, const SampleSpec& aSpec, const std::shared_ptr<FileFind::Match>& aMatch)
{
    HRESULT hr = E_FAIL;
    size_t sIndex = 0;

    for (const auto& anAttr : aMatch->MatchingAttributes)
    {
        SampleRef sampleRef;
        sampleRef.Matches.push_back(aMatch);

        sampleRef.VolumeSerial = aMatch->VolumeReader->VolumeSerialNumber();

        auto pSnapshotReader = std::dynamic_pointer_cast<SnapshotVolumeReader>(aMatch->VolumeReader);

        if (pSnapshotReader)
        {
            sampleRef.SnapshotID = pSnapshotReader->GetSnapshotID();
        }
        else
        {
            sampleRef.SnapshotID = GUID_NULL;
        }

        sampleRef.FRN = aMatch->FRN;
        sampleRef.InstanceID = anAttr.InstanceID;
        sampleRef.AttributeIndex = sIndex++;

        switch (status)
        {
            case NoLimits:
            case SampleWithinLimits:
                sampleRef.OffLimits = false;
                break;
            case GlobalSampleCountLimitReached:
            case GlobalMaxBytesPerSample:
            case GlobalMaxBytesTotal:
            case LocalSampleCountLimitReached:
            case LocalMaxBytesPerSample:
            case LocalMaxBytesTotal:
            case FailedToComputeLimits:
                sampleRef.OffLimits = true;
                break;
        }

        SampleSet::iterator prevSample = Samples.find(sampleRef);

        if (prevSample != end(Samples))
        {
            // this sample is already cabbed
            log::Verbose(
                _L_,
                L"Not adding duplicate sample %s to archive\r\n",
                aMatch->MatchingNames.front().FullPathName.c_str());
            SampleRef& item = const_cast<SampleRef&>(*prevSample);
            hr = S_FALSE;
        }
        else
        {
            for (auto& name : aMatch->MatchingNames)
            {
                log::Verbose(_L_, L"Adding sample %s to archive\r\n", name.FullPathName.c_str());

                sampleRef.Content = aSpec.Content;
                sampleRef.CollectionDate = CollectionDate;

                wstring CabSampleName;
                DWORD dwIdx = 0L;
                std::unordered_set<std::wstring>::iterator it;
                do
                {
                    if (FAILED(
                            hr = CreateSampleFileName(
                                sampleRef.Content, name.FILENAME(), anAttr.AttrName, dwIdx, CabSampleName)))
                        break;

                    if (!aSpec.Name.empty())
                    {
                        CabSampleName.insert(0, L"\\");
                        CabSampleName.insert(0, aSpec.Name);
                    }
                    it = SampleNames.find(CabSampleName);
                    dwIdx++;

                } while (it != end(SampleNames));

                SampleNames.insert(CabSampleName);
                sampleRef.SampleName = CabSampleName;
            }

            if (FAILED(hr = ConfigureSampleStreams(sampleRef)))
            {
                log::Error(_L_, hr, L"Failed to configure sample reference for %s\r\n", sampleRef.SampleName.c_str());
            }
            Samples.insert(sampleRef);
        }
    }

    if (hr == S_FALSE)
        return hr;
    return S_OK;
}

HRESULT Main::WriteSample(const std::shared_ptr<ArchiveCreate>& compressor, const SampleRef& sample) const
{
    if (sample.OffLimits)
    {
        return S_OK;
    }

    std::wstring strName;
    sample.Matches.front()->GetMatchFullName(
        sample.Matches.front()->MatchingNames.front(), sample.Matches.front()->MatchingAttributes.front(), strName);

    const auto itemArchivedCb = [this, &sample](HRESULT hrArchived) {
        FinalizeHashes(sample);

        HRESULT hr = AddSampleRefToCSV(m_tableWriter->GetTableOutput(), sample);
        if (FAILED(hr))
        {
            log::Error(
                _L_,
                hr,
                L"Failed to add sample %s metadata to csv\r\n",
                sample.Matches.front()->MatchingNames.front().FullPathName.c_str());
        }
    };

    HRESULT hr = compressor->AddStream(sample.SampleName.c_str(), strName.c_str(), sample.CopyStream, itemArchivedCb);
    if (FAILED(hr))
    {
        log::Error(_L_, hr, L"Failed to add sample %s\r\n", sample.SampleName.c_str());
    }

    return S_OK;
}

HRESULT Main::WriteSamples(const std::shared_ptr<ArchiveCreate>& compressor, const SampleSet& samples) const
{
    log::Info(_L_, L"\r\nAdding matching samples to archive:\r\n");

    for (const auto& sample : samples)
    {
        HRESULT hr = WriteSample(compressor, sample);
        if (FAILED(hr))
        {
            log::Error(_L_, hr, L"Failed to write sample '%s'", sample.SampleName.c_str());
            continue;
        }
    }

    return S_OK;
}

HRESULT Main::WriteSample(const std::filesystem::path& outputDir, const SampleRef& sample) const
{
    HRESULT hr = E_FAIL;

    if (sample.OffLimits)
    {
        return S_OK;
    }

    const fs::path sampleFile = outputDir / fs::path(sample.SampleName);

    std::error_code ec;
    fs::create_directories(sampleFile.parent_path(), ec);
    if (ec)
    {
        hr = HRESULT_FROM_WIN32(ec.value());
        log::Error(_L_, HRESULT_FROM_WIN32(ec.value()), L"Failed to create sample directory");
        return hr;
    }

    // As SampleSet is an std::set, iterator are const (key is built on value). It is safe to const_cast here
    // because the stream is not part of the key
    ByteStream& src = const_cast<ByteStream&>(*sample.CopyStream);
    hr = ::CopyStream(src, sampleFile, _L_);
    if (FAILED(hr))
    {
        log::Error(_L_, hr, L"Failed to copy stream of '%s'", sampleFile);
        return hr;
    }

    FinalizeHashes(sample);

    hr = AddSampleRefToCSV(m_tableWriter->GetTableOutput(), sample);
    if (FAILED(hr))
    {
        log::Error(
            _L_,
            hr,
            L"Failed to add sample %s metadata to csv\r\n",
            sample.Matches.front()->MatchingNames.front().FullPathName.c_str());
        return hr;
    }

    return S_OK;
}

HRESULT
Main::WriteSamples(const fs::path& outputDir, const SampleSet& samples) const
{
    log::Info(_L_, L"\r\nCopying matching samples to %s\r\n", outputDir.c_str());

    for (auto& sample : samples)
    {
        HRESULT hr = WriteSample(outputDir, sample);
        if (FAILED(hr))
        {
            log::Error(_L_, hr, L"Failed to write sample '%s'", sample.SampleName.c_str());
            continue;
        }

        log::Info(_L_, L"\t%s copied (%I64d bytes)\r\n", sample.SampleName.c_str(), sample.CopyStream->GetSize());
    }

    return S_OK;
}

void Main::FinalizeHashes(const Main::SampleRef& sample) const
{
    if (!sample.HashStream)
    {
        return;
    }

    if (sample.OffLimits && config.bReportAll && config.CryptoHashAlgs != CryptoHashStream::Algorithm::Undefined)
    {
        // Stream that were not collected must be read for HashStream
        ULONGLONG ullBytesWritten = 0LL;

        auto nullstream = DevNullStream();
        HRESULT hr = sample.CopyStream->CopyTo(nullstream, &ullBytesWritten);
        if (FAILED(hr))
        {
            Log::Error(L"Failed while computing hash of '{}' (code: {:#x})", sample.SampleName, hr);
        }

        sample.CopyStream->Close();
    }

    sample.HashStream->GetMD5(const_cast<CBinaryBuffer&>(sample.MD5));
    sample.HashStream->GetSHA1(const_cast<CBinaryBuffer&>(sample.SHA1));
    sample.HashStream->GetSHA256(const_cast<CBinaryBuffer&>(sample.SHA256));

    if (sample.FuzzyHashStream)
    {
        sample.FuzzyHashStream->GetSSDeep(const_cast<CBinaryBuffer&>(sample.SSDeep));
        sample.FuzzyHashStream->GetTLSH(const_cast<CBinaryBuffer&>(sample.TLSH));
    }
}

HRESULT Main::InitArchiveOutput()
{
    HRESULT hr = E_FAIL;

    const fs::path archivePath = config.Output.Path;
    auto tempDir = archivePath.parent_path();
    if (tempDir.empty())
    {
        std::error_code ec;
        tempDir = GetWorkingDirectoryApi(ec);
        if (ec)
        {
            Log::Warn(L"Failed to resolve current working directory (code: {:#x})", ec.value());
        }
    }

    ::CreateLogStream(tempDir / L"GetThisLogStream", hr, _L_);
    if (FAILED(hr))
    {
        log::Error(_L_, hr, L"Failed to create log stream\r\n");
        return hr;
    }

    m_compressor = ::CreateCompressor(config.Output, CompressorFlags::kNone, hr, _L_);
    if (m_compressor == nullptr)
    {
        Log::Error(L"Failed to create compressor");
        return hr;
    }

    m_tableWriter =
        ::CreateCsvWriter(tempDir / L"GetThisCsvStream", config.Output.Schema, config.Output.OutputEncoding, hr);
    if (m_tableWriter == nullptr)
    {
        Log::Error(L"Failed to create csv stream (code: {:#x})", hr);
        return hr;
    }

    return S_OK;
}

HRESULT Main::CloseArchiveOutput()
{
    _ASSERT(m_compressor);
    _ASSERT(m_tableWriter);

    m_compressor->FlushQueue();

    HRESULT hr = m_tableWriter->Flush();
    if (FAILED(hr))
    {
        log::Error(_L_, hr, L"Failed to flush csv writer\r\n");
    }

    auto tableStream = m_tableWriter->GetStream();
    if (tableStream && tableStream->GetSize())
    {
        hr = tableStream->SetFilePointer(0, FILE_BEGIN, nullptr);
        if (FAILED(hr))
        {
            log::Error(_L_, hr, L"Failed to rewind csv stream\r\n");
        }

        hr = m_compressor->AddStream(L"GetThis.csv", L"GetThis.csv", tableStream);
        if (FAILED(hr))
        {
            log::Error(_L_, hr, L"Failed to add GetThis.csv\r\n");
        }
    }

    auto logStream = _L_->GetByteStream();
    _L_->CloseLogToStream(false);

    if (logStream && logStream->GetSize() > 0LL)
    {
        hr = logStream->SetFilePointer(0, FILE_BEGIN, nullptr);
        if (FAILED(hr))
        {
            log::Error(_L_, hr, L"Failed to rewind log stream\r\n");
        }

        hr = m_compressor->AddStream(L"GetThis.log", L"GetThis.log", logStream);
        if (FAILED(hr))
        {
            log::Error(_L_, hr, L"Failed to add GetThis.log\r\n");
        }
    }

    hr = m_compressor->Complete();
    if (FAILED(hr))
    {
        log::Error(_L_, hr, L"Failed to complete %s\r\n", config.Output.Path.c_str());
        return hr;
    }

    hr = tableStream->Close();
    if (FAILED(hr))
    {
        log::Error(_L_, hr, L"Failed to close csv writer\r\n");
        return hr;
    }

    return S_OK;
}

HRESULT Main::InitDirectoryOutput()
{
    HRESULT hr = E_FAIL;
    std::error_code ec;

    const fs::path outputDir(config.Output.Path);
    fs::create_directories(outputDir, ec);
    if (ec)
    {
        hr = HRESULT_FROM_WIN32(ec.value());
        Log::Error(L"Failed to create output directory (code: {:#x})", hr);
        return hr;
    }

    m_tableWriter =
        ::CreateCsvWriter(outputDir / L"GetThis.csv", config.Output.Schema, config.Output.OutputEncoding, hr);
    if (m_tableWriter == nullptr)
    {
        Log::Error(L"Failed to create csv stream (code: {:#x})", hr);
        return hr;
    }

    return S_OK;
}

HRESULT Main::CloseDirectoryOutput()
{
    HRESULT hr = m_tableWriter->Flush();
    if (FAILED(hr))
    {
        Log::Error(L"Failed to flush table stream (code: {:#x})", hr);
        return hr;
    }

    hr = m_tableWriter->Close();
    if (FAILED(hr))
    {
        Log::Error(L"Failed to close table stream (code: {:#x})", hr);
        return hr;
    }

    return S_OK;
}

HRESULT Main::CollectSamples(const OutputSpec& output, const SampleSet& samples)
{
    if (OutputSpec::Archive == output.Type)
    {
        return WriteSamples(m_compressor, samples);
    }
    else if (OutputSpec::Directory == output.Type)
    {
        return WriteSamples(config.Output.Path, samples);
    }

    return E_NOTIMPL;
}

HRESULT Main::FindMatchingSamples()
{
    HRESULT hr = E_FAIL;

    hr = FileFinder.InitializeYara(config.Yara);
    if (FAILED(hr))
    {
        log::Error(_L_, hr, L"Failed to initialize Yara scan\r\n");
    }

    auto onMatchCb = [this, &hr](const std::shared_ptr<FileFind::Match>& aMatch, bool bStop) {
        _ASSERT(aMatch != nullptr);

        if (aMatch->MatchingAttributes.empty())
        {
            const std::wstring& name = aMatch->MatchingNames.front().FullPathName;
            log::Warning(
                _L_,
                E_FAIL,
                L"\"%s\" matched \"%s\" but no data related attribute was associated\r\n",
                name.c_str(),
                aMatch->Term->GetDescription().c_str());
            return;
        }

        // finding the corresponding Sample Spec (for limits)
        auto aSpecIt = std::find_if(
            std::begin(config.listofSpecs), std::end(config.listofSpecs), [aMatch](const SampleSpec& aSpec) -> bool {
                auto filespecIt = std::find(std::cbegin(aSpec.Terms), std::cend(aSpec.Terms), aMatch->Term);
                return filespecIt != std::cend(aSpec.Terms);
            });

        if (aSpecIt == std::end(config.listofSpecs))
        {
            log::Error(
                _L_,
                hr = E_FAIL,
                L"Could not find sample spec for match %s\r\n",
                aMatch->Term->GetDescription().c_str());
            return;
        }

        for (const auto& attr : aMatch->MatchingAttributes)
        {
            std::wstring strName;

            aMatch->GetMatchFullName(aMatch->MatchingNames.front(), attr, strName);

            DWORDLONG dwlDataSize = attr.DataStream->GetSize();
            LimitStatus status = SampleLimitStatus(GlobalLimits, aSpecIt->PerSampleLimits, dwlDataSize);

            hr = AddSamplesForMatch(status, *aSpecIt, aMatch);
            if (FAILED(hr))
            {
                log::Error(_L_, hr, L"\tFailed to add %s\r\n", strName.c_str());
            }

            switch (status)
            {
                case NoLimits:
                case SampleWithinLimits: {
                    if (hr == S_FALSE)
                    {
                        log::Info(_L_, L"\t%s is already collected\r\n", strName.c_str());
                    }
                    else
                    {
                        log::Info(_L_, L"\t%s matched (%d bytes)\r\n", strName.c_str(), dwlDataSize);

                        aSpecIt->PerSampleLimits.dwlAccumulatedBytesTotal += dwlDataSize;
                        aSpecIt->PerSampleLimits.dwAccumulatedSampleCount++;
                        GlobalLimits.dwlAccumulatedBytesTotal += dwlDataSize;
                        GlobalLimits.dwAccumulatedSampleCount++;
                    }
                }
                break;

                case GlobalSampleCountLimitReached:
                    log::Info(
                        _L_,
                        L"\t%s : Global sample count reached (%d)\r\n",
                        strName.c_str(),
                        GlobalLimits.dwMaxSampleCount);

                    GlobalLimits.bMaxSampleCountReached = true;
                    break;

                case GlobalMaxBytesPerSample:
                    log::Info(
                        _L_,
                        L"\t%s : Exceeds global per sample size limit (%I64d)\r\n",
                        strName.c_str(),
                        GlobalLimits.dwlMaxBytesPerSample);

                    GlobalLimits.bMaxBytesPerSampleReached = true;
                    break;

                case GlobalMaxBytesTotal:
                    log::Info(
                        _L_,
                        L"\t%s : Global total sample size limit reached (%I64d)\r\n",
                        strName.c_str(),
                        GlobalLimits.dwlMaxBytesTotal);

                    GlobalLimits.bMaxBytesTotalReached = true;
                    break;

                case LocalSampleCountLimitReached:
                    log::Info(
                        _L_,
                        L"\t%s : sample count reached (%d)\r\n",
                        strName.c_str(),
                        aSpecIt->PerSampleLimits.dwMaxSampleCount);

                    aSpecIt->PerSampleLimits.bMaxSampleCountReached = true;
                    break;

                case LocalMaxBytesPerSample:
                    log::Info(
                        _L_,
                        L"\t%s : Exceeds per sample size limit (%I64d)\r\n",
                        strName.c_str(),
                        aSpecIt->PerSampleLimits.dwlMaxBytesPerSample);

                    aSpecIt->PerSampleLimits.bMaxBytesPerSampleReached = true;
                    break;

                case LocalMaxBytesTotal:
                    log::Info(
                        _L_,
                        L"\t%s : total sample size limit reached (%I64d)\r\n",
                        strName.c_str(),
                        aSpecIt->PerSampleLimits.dwlMaxBytesTotal);

                    aSpecIt->PerSampleLimits.bMaxBytesTotalReached = true;
                    break;

                case FailedToComputeLimits:
                    break;
            }
        }
    };

    hr = FileFinder.Find(config.Locations, onMatchCb, false);
    if (FAILED(hr))
    {
        Log::Error(L"Failed while parsing locations");
    }

    return S_OK;
}

HRESULT Main::InitOutput()
{
    if (config.Output.Type == OutputSpec::Kind::Archive)
    {
        return InitArchiveOutput();
    }
    else if (config.Output.Type == OutputSpec::Kind::Directory)
    {
        return InitDirectoryOutput();
    }

    return E_NOTIMPL;
}

HRESULT Main::CloseOutput()
{
    HRESULT hr = E_FAIL;

    if (config.Output.Type == OutputSpec::Kind::Archive)
    {
        hr = CloseArchiveOutput();
        if (FAILED(hr))
        {
            Log::Error(L"Cannot close archive output (code: {:#x})", hr);
            return hr;
        }
    }
    else if (config.Output.Type == OutputSpec::Kind::Directory)
    {
        hr = CloseDirectoryOutput();
        if (FAILED(hr))
        {
            Log::Error(L"Cannot close directory output (code: {:#x})", hr);
            return hr;
        }
    }

    return hr;
}

HRESULT Main::Run()
{
    HRESULT hr = E_FAIL;
    LoadWinTrust();

    GetSystemTimeAsFileTime(&CollectionDate);

    try
    {
        if (config.bFlushRegistry)
        {
            if (FAILED(hr = RegFlushKeys()))
                log::Info(_L_, L"Failed to flush keys (hr = 0x%lx)\r\n", hr);
        }
    }
    catch (...)
    {
        log::Error(_L_, E_FAIL, L"GetThis failed during output setup, parameter output, RegistryFlush, exiting\r\n");
        return E_FAIL;
    }

    try
    {
        hr = InitOutput();
        if (FAILED(hr))
        {
            Log::Error(L"Cannot initialize output mode (code: {:#x})", hr);
            return hr;
        }

        hr = FindMatchingSamples();
        if (FAILED(hr))
        {
            Log::Error(L"GetThis failed while matching samples (code: {:#x})", hr);
            return hr;
        }

        hr = CollectSamples(config.Output, Samples);
        if (FAILED(hr))
        {
            log::Error(_L_, hr, L"\r\nGetThis failed while collecting samples\r\n");
            return hr;
        }

        hr = CloseOutput();
        if (FAILED(hr))
        {
            Log::Error(L"Failed to close output (code: {:#x})", hr);
        }
    }
    catch (...)
    {
        Log::Error(L"GetThis failed during sample collection, terminating archive");
        return E_ABORT;
    }

    return S_OK;
}

//
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright © 2011-2019 ANSSI. All Rights Reserved.
//
// Author(s): Jean Gautier (ANSSI)
//
#pragma once

#include "OrcLib.h"

#include <optional>
#include <exception>

#include <strsafe.h>

#include "Text/Fmt/std_optional.h"

//
// E_BOUNDS is not defined in SDK 7.1A
// C:\Program Files (x86)\Microsoft SDKs\Windows\v7.1A\Include\WinError.h
//
#ifndef E_BOUNDS
#    define E_BOUNDS _HRESULT_TYPEDEF_(0x8000000BL)
#endif

#pragma managed(push, off)

namespace Orc {

enum class Severity : short
{
    Unset,
    Fatal,
    Continue,
    NotImplemented
};

class Exception : public std::exception
{
public:
    template <typename... Args>
    Exception(Severity status, _In_ HRESULT hr, std::wstring_view fmt, Args&&... args)
        : m_severity(status)
        , m_ec(SystemError(hr))
    {
        Description = fmt::format(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    Exception(Severity status, _In_ std::error_code ec, std::wstring_view fmt, Args&&... args)
        : m_severity(status)
        , m_ec(ec)
    {
        Description = fmt::format(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    Exception(Severity status, std::wstring_view fmt, Args&&... args)
        : m_severity(status)
    {
        Description = fmt::format(fmt, std::forward<Args>(args)...);
    }

    Exception() = default;
    Exception(Severity status)
        : m_severity(status)
    {
    }
    Exception(std::wstring descr);

    Exception(Severity status, _In_ HRESULT hr)
        : m_severity(status)
        , m_ec(SystemError(hr))
    {
    }
    Exception(Severity status, _In_ std::error_code ec)
        : m_severity(status)
        , m_ec(std::move(ec))
    {
    }

    Exception(Exception&& other) noexcept = default;
    Exception(const Exception& other) = default;
    Exception& operator=(Exception&& other) noexcept = default;
    Exception& operator=(const Exception& other) = default;

    void SetErrorCode(_In_ std::error_code ec) { std::swap(m_ec, ec); }
    const std::error_code& ErrorCode() const { return m_ec; }

    HRESULT SetHRESULT(_In_ HRESULT Status)
    {
        m_ec = SystemError(Status);
        return Status;
    }

    bool IsCritical() const { return m_severity == Severity::Fatal; }

    Severity m_severity = Severity::Unset;
    std::wstring Description;
    std::error_code m_ec;

    HRESULT PrintMessage() const;

    virtual const char* what() const override;

    virtual ~Exception();

private:
    mutable std::optional<std::string> What;
};

}  // namespace Orc

#pragma managed(pop)

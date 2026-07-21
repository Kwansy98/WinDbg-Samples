//----------------------------------------------------------------------------
//
//  LiveExdiGdbSrvServer.cpp  Implementation of CLiveExdiGdbSrvServerclass
//  This class implements the following interfaces:
//		[default] interface IeXdiServer3;
//      interface IeXdiARM4Context3;
//      interface IeXdiX86_64Context3;
//		interface IeXdiX86ExContext3;
//      interface IAsynchronousCommandNotificationReceiver;
//
// Copyright (c) Microsoft. All rights reserved.
//----------------------------------------------------------------------------

#include "stdafx.h"
#include "LiveExdiGdbSrvServer.h"
#include "ComHelpers.h"
#include "AsynchronousGdbSrvController.h"
#include "CommandLogger.h"
#include "ArgumentHelpers.h"
#include "ExceptionHelpers.h"
#include "GdbSrvRspClient.h"
#include "BasicExdiBreakpoint.h"
#include "dbgeng_exdi_io.h"
#include "cfgExdiGdbSrvHelper.h"
#include <string>
#include <algorithm>
#include <vector>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <intrin.h>

#define METHOD_NOT_IMPLEMENTED if (IsDebuggerPresent()) \
                                   __debugbreak(); \
                               return E_NOTIMPL

using namespace GdbSrvControllerLib;

//=============================================================================
// Private defines and typedefs
//=============================================================================
//  AMD64 Context Flag definitions
#define AMD64_CONTEXT_AMD64             0x00100000L
#define AMD64_CONTEXT_CONTROL           (AMD64_CONTEXT_AMD64 | 0x00000001L)
#define AMD64_CONTEXT_INTEGER           (AMD64_CONTEXT_AMD64 | 0x00000002L)
#define AMD64_CONTEXT_SEGMENTS          (AMD64_CONTEXT_AMD64 | 0x00000004L)
#define AMD64_CONTEXT_FLOATING_POINT    (AMD64_CONTEXT_AMD64 | 0x00000008L)
#define AMD64_CONTEXT_DEBUG_REGISTERS   (AMD64_CONTEXT_AMD64 | 0x00000010L)
#define AMD64_CONTEXT_FULL \
    (AMD64_CONTEXT_CONTROL | AMD64_CONTEXT_INTEGER | AMD64_CONTEXT_FLOATING_POINT)

//  Used to allow correctly processing of the Segment descriptors by the disassembler
#define X86_DESC_PRESENT                0x80
#define X86_DESC_LONG_MODE              0x200
#define X86_DESC_DEFAULT_BIG            0x400
#define SEGDESC_INVALID                 0xffffffff
#define X86_DESC_FLAGS                  (X86_DESC_DEFAULT_BIG | X86_DESC_PRESENT)


//=============================================================================
// Global data definitions
//=============================================================================
//  Connection server string
const DWORD s_ConnectionCookie = 'SMPL';

//  SSE register list
const char * s_sseRegList[] = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"};
//  Number of SSE registers
const int s_numberOfSseRegisters = (ARRAYSIZE(s_sseRegList));
//  SSE x64 register list
const char* s_sseX64RegList[] = { "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
                                  "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15" };
//  Number of SSE registers
const int s_numberOfSseX64Registers = (ARRAYSIZE(s_sseX64RegList));

//  80387 coprocessor register info.
const int s_numberOfCoprocessorRegisters = 8;
const int s_numberOfBytesCoprocessorRegister = (SIZE_OF_80387_REGISTERS_IN_BYTES / s_numberOfCoprocessorRegisters);
const char * s_fpRegList[] = {"st0", "st1", "st2", "st3", "st4", "st5", "st6", "st7"};
const int s_numberFPRegList = (ARRAYSIZE(s_fpRegList));

namespace
{
    thread_local std::unordered_map<UINT_PTR, CLiveExdiGdbSrvServer*> g_timerOwners;

    char ToAsciiHex(_In_ unsigned char value)
    {
        return static_cast<char>(value < 10 ? ('0' + value) : ('A' + value - 10));
    }

    bool TryDecodeAsciiHex(_In_ char value, _Out_ unsigned char* decoded)
    {
        if (value >= '0' && value <= '9')
        {
            *decoded = static_cast<unsigned char>(value - '0');
            return true;
        }

        if (value >= 'a' && value <= 'f')
        {
            *decoded = static_cast<unsigned char>(value - 'a' + 10);
            return true;
        }

        if (value >= 'A' && value <= 'F')
        {
            *decoded = static_cast<unsigned char>(value - 'A' + 10);
            return true;
        }

        return false;
    }

    void AppendVMwareLog(_In_z_ const char* format, ...)
    {
        char tempPath[MAX_PATH] = {};
        if (GetTempPathA(_countof(tempPath), tempPath) == 0)
        {
            return;
        }

        char logPath[MAX_PATH] = {};
        if (sprintf_s(logPath, _countof(logPath), "%sExdiGdbSrv-vmware.log", tempPath) <= 0)
        {
            return;
        }

        FILE* logFile = nullptr;
        if (fopen_s(&logFile, logPath, "a") != 0 || logFile == nullptr)
        {
            return;
        }

        va_list args;
        va_start(args, format);
        vfprintf(logFile, format, args);
        va_end(args);
        fclose(logFile);
    }

    bool IsCurrentTargetVMware()
    {
        std::wstring targetName;
        ConfigExdiGdbServerHelper::GetInstanceCfgExdiGdbServer(nullptr).GetGdbServerTargetName(targetName);
        return _wcsicmp(targetName.c_str(), L"VMWare") == 0 ||
               _wcsicmp(targetName.c_str(), L"VMware") == 0;
    }

    bool TryParseHexValue(_In_ const std::string& text, _In_ const char* marker, _Out_ ULONG64* value)
    {
        size_t valueOffset = text.find(marker);
        if (valueOffset == std::string::npos)
        {
            return false;
        }

        valueOffset += strlen(marker);
        while (valueOffset < text.length() &&
               (text[valueOffset] == ' ' ||
                text[valueOffset] == '\t' ||
                text[valueOffset] == ':' ||
                text[valueOffset] == '='))
        {
            ++valueOffset;
        }

        if (valueOffset + 1 < text.length() &&
            text[valueOffset] == '0' &&
            (text[valueOffset + 1] == 'x' || text[valueOffset + 1] == 'X'))
        {
            valueOffset += 2;
        }

        std::string digits;
        for (; valueOffset < text.length(); ++valueOffset)
        {
            char ch = text[valueOffset];
            if (ch == '`')
            {
                continue;
            }

            if (!std::isxdigit(static_cast<unsigned char>(ch)))
            {
                break;
            }

            digits.push_back(ch);
        }

        if (digits.empty())
        {
            return false;
        }

        *value = _strtoui64(digits.c_str(), nullptr, 16);
        return true;
    }

    bool TryBuildMonitorCommand(_In_ LPCWSTR command, _Out_ std::string* encodedCommand)
    {
        char narrowCommand[128] = {};
        if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, command, -1, narrowCommand, _countof(narrowCommand), nullptr, nullptr) == 0)
        {
            return false;
        }

        encodedCommand->assign("qRcmd,");
        for (const char* cursor = narrowCommand; *cursor != '\0'; ++cursor)
        {
            unsigned char value = static_cast<unsigned char>(*cursor);
            encodedCommand->push_back(ToAsciiHex((value >> 4) & 0x0f));
            encodedCommand->push_back(ToAsciiHex(value & 0x0f));
        }

        return true;
    }

    bool TryDecodeConsoleOutputPacket(_In_ const std::string& reply, _Out_ std::string* output)
    {
        if (reply.length() < 3 || reply[0] != 'O')
        {
            return false;
        }

        output->clear();
        for (size_t index = 1; index + 1 < reply.length(); index += 2)
        {
            unsigned char high = 0;
            unsigned char low = 0;
            if (!TryDecodeAsciiHex(reply[index], &high) ||
                !TryDecodeAsciiHex(reply[index + 1], &low))
            {
                return false;
            }

            output->push_back(static_cast<char>((high << 4) | low));
        }

        return true;
    }

    bool IsExpectedVMwareMonitorOutput(_In_ const std::string& output, _In_ const char* expectedToken)
    {
        size_t offset = 0;
        while (offset < output.length() && std::isspace(static_cast<unsigned char>(output[offset])))
        {
            ++offset;
        }

        size_t tokenLength = strlen(expectedToken);
        return output.length() >= offset + tokenLength &&
               _strnicmp(output.c_str() + offset, expectedToken, tokenLength) == 0;
    }

    bool QueryVMwareMonitor(
        _In_ GdbSrvController* pController,
        _In_ LPCWSTR command,
        _In_ const char* expectedToken,
        _Out_ std::string* output)
    {
        std::string encodedCommand;
        if (!TryBuildMonitorCommand(command, &encodedCommand))
        {
            return false;
        }

        unsigned activeCpu = pController->GetLastKnownActiveCpu();
        std::string reply = pController->ExecuteCommandOnProcessor(
            encodedCommand.c_str(),
            true,
            0,
            activeCpu);

        char commandText[128] = {};
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, command, -1, commandText, _countof(commandText), nullptr, nullptr);
        bool foundExpectedOutput = false;
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            if (reply == "OK")
            {
                AppendVMwareLog("monitor '%s' attempt=%d raw='OK' completed=%d\n", commandText, attempt, foundExpectedOutput ? 1 : 0);
                if (foundExpectedOutput)
                {
                    return true;
                }
            }

            bool decoded = TryDecodeConsoleOutputPacket(reply, output);
            AppendVMwareLog("monitor '%s' attempt=%d raw='%s' decoded='%s'\n", commandText, attempt, reply.c_str(), decoded ? output->c_str() : "");
            if (decoded && IsExpectedVMwareMonitorOutput(*output, expectedToken))
            {
                foundExpectedOutput = true;
            }

            try
            {
                reply = pController->GetResponseOnProcessor(0, activeCpu);
            }
            catch (const _com_error& error)
            {
                AppendVMwareLog("monitor '%s' no matching response, hr=%08x\n", commandText, error.Error());
                return false;
            }
        }

        AppendVMwareLog("monitor '%s' exhausted stale responses completed=%d\n", commandText, foundExpectedOutput ? 1 : 0);
        return false;
    }

    bool ExecuteVMwareMonitorCommand(
        _In_ GdbSrvController* pController,
        _In_ LPCWSTR command)
    {
        std::string encodedCommand;
        if (!TryBuildMonitorCommand(command, &encodedCommand))
        {
            return false;
        }

        unsigned activeCpu = pController->GetLastKnownActiveCpu();
        std::string reply = pController->ExecuteCommandOnProcessor(
            encodedCommand.c_str(),
            true,
            0,
            activeCpu);

        char commandText[128] = {};
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, command, -1, commandText, _countof(commandText), nullptr, nullptr);
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            if (reply == "OK")
            {
                return true;
            }

            if (!reply.empty() && reply[0] == 'E')
            {
                AppendVMwareLog("monitor '%s' failed raw='%s'\n", commandText, reply.c_str());
                return false;
            }

            try
            {
                reply = pController->GetResponseOnProcessor(0, activeCpu);
            }
            catch (const _com_error& error)
            {
                AppendVMwareLog("monitor '%s' no OK, hr=%08x\n", commandText, error.Error());
                return false;
            }
        }

        AppendVMwareLog("monitor '%s' exhausted waiting for OK\n", commandText);
        return false;
    }

    bool DecodeHexMemoryPacket(
        _In_ const std::string& reply,
        _Out_writes_bytes_(size) void* data,
        _In_ size_t size)
    {
        if (reply.length() < size * 2 ||
            (!reply.empty() && reply[0] == 'E'))
        {
            return false;
        }

        BYTE* bytes = static_cast<BYTE*>(data);
        for (size_t index = 0; index < size; ++index)
        {
            unsigned char high = 0;
            unsigned char low = 0;
            if (!TryDecodeAsciiHex(reply[index * 2], &high) ||
                !TryDecodeAsciiHex(reply[(index * 2) + 1], &low))
            {
                return false;
            }

            bytes[index] = static_cast<BYTE>((high << 4) | low);
        }

        return true;
    }

    bool ReadVMwarePhysicalMemory(
        _In_ GdbSrvController* pController,
        _In_ ADDRESS_TYPE physicalAddress,
        _Out_writes_bytes_(size) void* data,
        _In_ size_t size)
    {
        if (!ExecuteVMwareMonitorCommand(pController, L"phys"))
        {
            AppendVMwareLog("failed to enter VMware physical memory mode\n");
            return false;
        }

        constexpr size_t maxPhysicalReadChunk = 0x100;
        BYTE* output = static_cast<BYTE*>(data);
        size_t bytesLeft = size;
        ADDRESS_TYPE currentAddress = physicalAddress;
        bool readOk = true;

        while (bytesLeft != 0)
        {
            const size_t bytesToRead = (std::min)(bytesLeft, maxPhysicalReadChunk);
            char command[64] = {};
            sprintf_s(command, _countof(command), "m%I64x,%x", static_cast<ULONGLONG>(currentAddress), static_cast<unsigned>(bytesToRead));

            std::string reply;
            try
            {
                reply = pController->ExecuteCommandOnProcessor(
                    command,
                    true,
                    (bytesToRead * 2) + 256,
                    pController->GetLastKnownActiveCpu());
                if (!DecodeHexMemoryPacket(reply, output, bytesToRead))
                {
                    AppendVMwareLog(
                        "VMware physical read bad reply pa=%I64x size=%Iu raw='%s'\n",
                        static_cast<ULONGLONG>(currentAddress),
                        bytesToRead,
                        reply.c_str());
                    readOk = false;
                    break;
                }
            }
            catch (const _com_error& error)
            {
                AppendVMwareLog(
                    "VMware physical read exception pa=%I64x size=%Iu hr=%08x\n",
                    static_cast<ULONGLONG>(currentAddress),
                    bytesToRead,
                    error.Error());
                readOk = false;
                break;
            }

            output += bytesToRead;
            currentAddress += bytesToRead;
            bytesLeft -= bytesToRead;
        }

        if (!ExecuteVMwareMonitorCommand(pController, L"virt"))
        {
            AppendVMwareLog("failed to restore VMware virtual memory mode\n");
            return false;
        }

        return readOk;
    }

    void QueryVMwareScalarRegister(
        _In_ GdbSrvController* pController,
        _In_ LPCWSTR command,
        _In_ const char* registerName,
        _Inout_ std::map<std::string, std::string>& registers)
    {
        std::string output;
        ULONG64 value = 0;
        if (QueryVMwareMonitor(pController, command, registerName, &output) &&
            TryParseHexValue(output, "=", &value))
        {
            char valueString[32] = {};
            sprintf_s(valueString, _countof(valueString), "%I64x", value);
            registers[registerName] = valueString;
        }
    }

    void QueryVMwareDescriptorRegister(
        _In_ GdbSrvController* pController,
        _In_ LPCWSTR command,
        _In_ const char* baseName,
        _In_ const char* limitName,
        _Inout_ std::map<std::string, std::string>& registers)
    {
        std::string output;
        ULONG64 base = 0;
        ULONG64 limit = 0;
        const char* expectedToken = strncmp(baseName, "gdtr", 4) == 0 ? "gdtr" : "idtr";
        if (QueryVMwareMonitor(pController, command, expectedToken, &output) &&
            TryParseHexValue(output, "base", &base) &&
            TryParseHexValue(output, "limit", &limit))
        {
            char valueString[32] = {};
            sprintf_s(valueString, _countof(valueString), "%I64x", base);
            registers[baseName] = valueString;

            sprintf_s(valueString, _countof(valueString), "%I64x", limit);
            registers[limitName] = valueString;
            AppendVMwareLog("parsed %s=%s %s=%s\n", baseName, registers[baseName].c_str(), limitName, registers[limitName].c_str());
        }
        else
        {
            AppendVMwareLog("failed to parse descriptor output '%s'\n", output.c_str());
        }
    }

    void QueryVMwareSpecialRegisters(
        _In_ GdbSrvController* pController,
        _Inout_ std::map<std::string, std::string>& registers)
    {
        QueryVMwareScalarRegister(pController, L"r cr0", "cr0", registers);
        QueryVMwareScalarRegister(pController, L"r cr2", "cr2", registers);
        QueryVMwareScalarRegister(pController, L"r cr3", "cr3", registers);
        QueryVMwareScalarRegister(pController, L"r cr4", "cr4", registers);
        QueryVMwareScalarRegister(pController, L"r cr8", "cr8", registers);
        QueryVMwareDescriptorRegister(pController, L"r gdtr", "gdtrbase", "gdtrlimit", registers);
        QueryVMwareDescriptorRegister(pController, L"r idtr", "idtrbase", "idtrlimit", registers);
    }

    bool ReadTargetMemory(
        _In_ GdbSrvController* pController,
        _In_ ADDRESS_TYPE address,
        _Out_writes_bytes_(size) void* data,
        _In_ size_t size,
        _In_ const memoryAccessType memoryType)
    {
        try
        {
            SimpleCharBuffer buffer = pController->ReadMemory(address, size, memoryType);
            if (buffer.GetLength() != size)
            {
                return false;
            }

            memcpy(data, buffer.GetInternalBuffer(), size);
            return true;
        }
        catch (const _com_error&)
        {
            return false;
        }
    }

    bool ReadTargetPhysicalMemory(
        _In_ GdbSrvController* pController,
        _In_ ADDRESS_TYPE physicalAddress,
        _Out_writes_bytes_(size) void* data,
        _In_ size_t size)
    {
        if (IsCurrentTargetVMware())
        {
            return ReadVMwarePhysicalMemory(pController, physicalAddress, data, size);
        }

        memoryAccessType physicalMemory = {};
        physicalMemory.isPhysical = 1;
        return ReadTargetMemory(pController, physicalAddress, data, size, physicalMemory);
    }

    bool ReadTargetPhysicalU64(
        _In_ GdbSrvController* pController,
        _In_ ADDRESS_TYPE physicalAddress,
        _Out_ ULONGLONG* value)
    {
        *value = 0;
        return ReadTargetPhysicalMemory(pController, physicalAddress, value, sizeof(*value));
    }

    bool TryTranslateX64VirtualAddress(
        _In_ GdbSrvController* pController,
        _In_ ADDRESS_TYPE cr3,
        _In_ ADDRESS_TYPE virtualAddress,
        _Out_ ADDRESS_TYPE* physicalAddress)
    {
        constexpr ULONGLONG pageOffsetMask = 0xfff;
        constexpr ULONGLONG pageFrameMask = 0x000ffffffffff000ULL;
        constexpr ULONGLONG largePage2MbMask = 0x000fffffffe00000ULL;
        constexpr ULONGLONG largePage1GbMask = 0x000fffffc0000000ULL;
        constexpr ULONGLONG presentBit = 1;
        constexpr ULONGLONG largePageBit = 1ULL << 7;

        if (cr3 == 0 || physicalAddress == nullptr)
        {
            return false;
        }

        ULONGLONG pml4 = static_cast<ULONGLONG>(cr3) & pageFrameMask;
        ULONGLONG pml4e = 0;
        ULONGLONG pdpte = 0;
        ULONGLONG pde = 0;
        ULONGLONG pte = 0;
        const ULONGLONG va = static_cast<ULONGLONG>(virtualAddress);

        const ULONGLONG pml4Index = (va >> 39) & 0x1ff;
        const ULONGLONG pdptIndex = (va >> 30) & 0x1ff;
        const ULONGLONG pdIndex = (va >> 21) & 0x1ff;
        const ULONGLONG ptIndex = (va >> 12) & 0x1ff;
        const ADDRESS_TYPE pml4eAddress = static_cast<ADDRESS_TYPE>(pml4 + (pml4Index * sizeof(ULONGLONG)));

        if (!ReadTargetPhysicalU64(pController, pml4eAddress, &pml4e))
        {
            AppendVMwareLog("vtop pml4 read failed cr3=%I64x va=%I64x pml4=%I64x index=%I64x entryPa=%I64x\n",
                static_cast<ULONGLONG>(cr3),
                va,
                pml4,
                pml4Index,
                static_cast<ULONGLONG>(pml4eAddress));
            return false;
        }

        if ((pml4e & presentBit) == 0)
        {
            return false;
        }

        const ADDRESS_TYPE pdpteAddress = static_cast<ADDRESS_TYPE>((pml4e & pageFrameMask) + (pdptIndex * sizeof(ULONGLONG)));
        if (!ReadTargetPhysicalU64(pController, pdpteAddress, &pdpte))
        {
            AppendVMwareLog("vtop pdpt read failed cr3=%I64x va=%I64x index=%I64x entryPa=%I64x\n",
                static_cast<ULONGLONG>(cr3),
                va,
                pdptIndex,
                static_cast<ULONGLONG>(pdpteAddress));
            return false;
        }

        if ((pdpte & presentBit) == 0)
        {
            return false;
        }

        if ((pdpte & largePageBit) != 0)
        {
            *physicalAddress = static_cast<ADDRESS_TYPE>((pdpte & largePage1GbMask) + (va & 0x3fffffffULL));
            return true;
        }

        const ADDRESS_TYPE pdeAddress = static_cast<ADDRESS_TYPE>((pdpte & pageFrameMask) + (pdIndex * sizeof(ULONGLONG)));
        if (!ReadTargetPhysicalU64(pController, pdeAddress, &pde))
        {
            AppendVMwareLog("vtop pd read failed cr3=%I64x va=%I64x index=%I64x entryPa=%I64x\n",
                static_cast<ULONGLONG>(cr3),
                va,
                pdIndex,
                static_cast<ULONGLONG>(pdeAddress));
            return false;
        }

        if ((pde & presentBit) == 0)
        {
            return false;
        }

        if ((pde & largePageBit) != 0)
        {
            *physicalAddress = static_cast<ADDRESS_TYPE>((pde & largePage2MbMask) + (va & 0x1fffffULL));
            return true;
        }

        const ADDRESS_TYPE pteAddress = static_cast<ADDRESS_TYPE>((pde & pageFrameMask) + (ptIndex * sizeof(ULONGLONG)));
        if (!ReadTargetPhysicalU64(pController, pteAddress, &pte))
        {
            AppendVMwareLog("vtop pt read failed cr3=%I64x va=%I64x index=%I64x entryPa=%I64x\n",
                static_cast<ULONGLONG>(cr3),
                va,
                ptIndex,
                static_cast<ULONGLONG>(pteAddress));
            return false;
        }

        if ((pte & presentBit) == 0)
        {
            return false;
        }

        *physicalAddress = static_cast<ADDRESS_TYPE>((pte & pageFrameMask) + (va & pageOffsetMask));
        return true;
    }

    bool ReadTargetVirtualMemory(
        _In_ GdbSrvController* pController,
        _In_ ADDRESS_TYPE cr3,
        _In_ ADDRESS_TYPE virtualAddress,
        _Out_writes_bytes_(size) void* data,
        _In_ size_t size)
    {
        constexpr ADDRESS_TYPE pageSize = 0x1000;
        BYTE* output = static_cast<BYTE*>(data);
        ADDRESS_TYPE address = virtualAddress;
        size_t bytesLeft = size;

        while (bytesLeft != 0)
        {
            ADDRESS_TYPE physicalAddress = 0;
            if (!TryTranslateX64VirtualAddress(pController, cr3, address, &physicalAddress))
            {
                return false;
            }

            size_t pageBytesLeft = static_cast<size_t>(pageSize - (address & (pageSize - 1)));
            size_t bytesToRead = (std::min)(bytesLeft, pageBytesLeft);
            if (!ReadTargetPhysicalMemory(pController, physicalAddress, output, bytesToRead))
            {
                return false;
            }

            output += bytesToRead;
            address += bytesToRead;
            bytesLeft -= bytesToRead;
        }

        return true;
    }

    bool ReadTargetVirtualMemoryBuffer(
        _In_ GdbSrvController* pController,
        _In_ ADDRESS_TYPE cr3,
        _In_ ADDRESS_TYPE virtualAddress,
        _In_ size_t size,
        _Out_ SimpleCharBuffer* buffer)
    {
        if (!buffer->TryEnsureCapacity(size))
        {
            throw _com_error(E_OUTOFMEMORY);
        }

        buffer->SetLength(size);
        if (size == 0)
        {
            return true;
        }

        return ReadTargetVirtualMemory(pController, cr3, virtualAddress, buffer->GetInternalBuffer(), size);
    }

    bool IsKernelImageCandidate(
        _In_ GdbSrvController* pController,
        _In_ ADDRESS_TYPE imageBase,
        _In_ ADDRESS_TYPE cr3,
        _In_ ADDRESS_TYPE codeAddress)
    {
        IMAGE_DOS_HEADER dosHeader = {};
        if (!ReadTargetVirtualMemory(pController, cr3, imageBase, &dosHeader, sizeof(dosHeader)) ||
            dosHeader.e_magic != IMAGE_DOS_SIGNATURE ||
            dosHeader.e_lfanew <= 0 ||
            dosHeader.e_lfanew >= 0x100000)
        {
            return false;
        }

        IMAGE_NT_HEADERS64 ntHeaders = {};
        if (!ReadTargetVirtualMemory(pController, cr3, imageBase + dosHeader.e_lfanew, &ntHeaders, sizeof(ntHeaders)) ||
            ntHeaders.Signature != IMAGE_NT_SIGNATURE ||
            ntHeaders.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            ntHeaders.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            ntHeaders.OptionalHeader.Subsystem != IMAGE_SUBSYSTEM_NATIVE ||
            ntHeaders.OptionalHeader.SizeOfImage < 0x100000 ||
            ntHeaders.OptionalHeader.SizeOfImage > 0x40000000 ||
            ntHeaders.OptionalHeader.SectionAlignment < 0x1000 ||
            ntHeaders.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
        {
            return false;
        }

        const DWORD sectionCount = (std::min)(static_cast<DWORD>(ntHeaders.FileHeader.NumberOfSections), static_cast<DWORD>(32));
        std::vector<IMAGE_SECTION_HEADER> sections(sectionCount);
        ADDRESS_TYPE sectionHeadersAddress = imageBase + dosHeader.e_lfanew + FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader) + ntHeaders.FileHeader.SizeOfOptionalHeader;
        if (sectionCount == 0 ||
            !ReadTargetVirtualMemory(pController, cr3, sectionHeadersAddress, sections.data(), sections.size() * sizeof(IMAGE_SECTION_HEADER)))
        {
            return false;
        }

        bool containsCodeAddress = false;
        for (const IMAGE_SECTION_HEADER& section : sections)
        {
            DWORD sectionSize = (std::max)(static_cast<DWORD>(section.Misc.VirtualSize), static_cast<DWORD>(section.SizeOfRawData));
            ADDRESS_TYPE sectionStart = imageBase + section.VirtualAddress;
            ADDRESS_TYPE sectionEnd = sectionStart + sectionSize;
            if ((section.Characteristics & (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE)) != 0 &&
                codeAddress >= sectionStart &&
                codeAddress < sectionEnd)
            {
                containsCodeAddress = true;
                break;
            }
        }

        if (!containsCodeAddress)
        {
            return false;
        }

        AppendVMwareLog(
            "kernel PE candidate base=%I64x size=%x entry=%x imagebase=%I64x\n",
            static_cast<ULONGLONG>(imageBase),
            ntHeaders.OptionalHeader.SizeOfImage,
            ntHeaders.OptionalHeader.AddressOfEntryPoint,
            ntHeaders.OptionalHeader.ImageBase);
        return true;
    }

    HRESULT FindNtImageBaseNearAddress(
        _In_ GdbSrvController* pController,
        _In_ ADDRESS_TYPE codeAddress,
        _In_ ADDRESS_TYPE cr3,
        _In_z_ const char* sourceName,
        _Out_ ADDRESS_TYPE* ntBaseAddress)
    {
        if (codeAddress == 0 || cr3 == 0 || ntBaseAddress == nullptr)
        {
            return E_INVALIDARG;
        }

        constexpr ULONGLONG pageSize = 0x10000;
        ULONGLONG searchAddress = static_cast<ULONGLONG>(codeAddress) & ~(pageSize - 1);
        constexpr ULONGLONG maxSearchDistance = 0x08000000;
        constexpr ULONGLONG minimumKernelBase = 0xFFFFF80000000000ULL;
        ULONGLONG searchedDistance = 0;
        AppendVMwareLog("ntbase scan source=%s start=%I64x max=%I64x step=%I64x\n", sourceName, searchAddress, maxSearchDistance, pageSize);
        while (searchAddress >= minimumKernelBase && searchedDistance < maxSearchDistance)
        {
            if (IsKernelImageCandidate(pController, static_cast<ADDRESS_TYPE>(searchAddress), cr3, codeAddress))
            {
                *ntBaseAddress = static_cast<ADDRESS_TYPE>(searchAddress);
                AppendVMwareLog("ntbase found source=%s base=%I64x\n", sourceName, searchAddress);
                return S_OK;
            }

            searchAddress -= pageSize;
            searchedDistance += pageSize;
            if ((searchedDistance & 0x00FFFFFFULL) == 0)
            {
                AppendVMwareLog("ntbase scan progress source=%s distance=%I64x address=%I64x\n", sourceName, searchedDistance, searchAddress);
            }
        }

        AppendVMwareLog("ntbase scan failed source=%s distance=%I64x\n", sourceName, searchedDistance);
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    HRESULT FindNtBaseAddressFromIdt(
        _In_ GdbSrvController* pController,
        _In_ ADDRESS_TYPE idtBase,
        _In_ ADDRESS_TYPE cr3,
        _Out_ ADDRESS_TYPE* ntBaseAddress)
    {
        if (idtBase == 0 || cr3 == 0 || ntBaseAddress == nullptr)
        {
            return E_INVALIDARG;
        }

        WORD idtEntry[8] = {};
        if (!ReadTargetVirtualMemory(pController, cr3, idtBase, idtEntry, sizeof(idtEntry)))
        {
            AppendVMwareLog("failed to read IDT base=%I64x\n", static_cast<ULONGLONG>(idtBase));
            return E_FAIL;
        }

        ADDRESS_TYPE interruptHandler =
            (static_cast<ADDRESS_TYPE>(idtEntry[5]) << 48) |
            (static_cast<ADDRESS_TYPE>(idtEntry[4]) << 32) |
            (static_cast<ADDRESS_TYPE>(idtEntry[3]) << 16) |
            static_cast<ADDRESS_TYPE>(idtEntry[0]);
        AppendVMwareLog("IDT[0] handler=%I64x\n", static_cast<ULONGLONG>(interruptHandler));
        return FindNtImageBaseNearAddress(pController, interruptHandler, cr3, "idt", ntBaseAddress);
    }

    HRESULT FindNtBaseAddressFromContext(
        _In_ GdbSrvController* pController,
        _In_ const CONTEXT_X86_64& currentContext,
        _Out_ ADDRESS_TYPE* ntBaseAddress)
    {
        if (currentContext.IDTBase == 0)
        {
            AppendVMwareLog("IDT base unavailable for NT base lookup\n");
            return E_INVALIDARG;
        }

        if (currentContext.RegCr3 == 0)
        {
            AppendVMwareLog("CR3 unavailable for NT base lookup\n");
            return E_INVALIDARG;
        }

        return FindNtBaseAddressFromIdt(pController, currentContext.IDTBase, currentContext.RegCr3, ntBaseAddress);
    }

    struct KdVersionBlock64
    {
        USHORT MajorVersion;
        USHORT MinorVersion;
        UCHAR ProtocolVersion;
        UCHAR KdSecondaryVersion;
        USHORT Flags;
        USHORT MachineType;
        UCHAR MaxPacketType;
        UCHAR MaxStateChange;
        UCHAR MaxManipulate;
        UCHAR Simulation;
        USHORT Unused;
        ULONGLONG KernBase;
        ULONGLONG PsLoadedModuleList;
        ULONGLONG DebuggerDataList;
    };

    struct DebugDataHeader64
    {
        ULONGLONG Flink;
        ULONGLONG Blink;
        ULONG OwnerTag;
        ULONG Size;
    };

    struct ListEntry64
    {
        ULONGLONG Flink;
        ULONGLONG Blink;
    };

    struct WindowsDebuggerData
    {
        ADDRESS_TYPE versionAddress;
        KdVersionBlock64 version;
        ADDRESS_TYPE debuggerDataAddress;
        std::vector<BYTE> decodedDebuggerData;
    };

    static_assert(sizeof(KdVersionBlock64) == 0x28, "Unexpected DBGKD_GET_VERSION64 layout");
    static_assert(sizeof(DebugDataHeader64) == 0x18, "Unexpected DBGKD_DEBUG_DATA_HEADER64 layout");

    constexpr USHORT kdMajorVersionNt = 0x000f;
    constexpr UCHAR kdProtocolVersion = 6;
    constexpr ULONG kdDebuggerDataOwnerTag = 0x4742444b; // 'KDBG'
    constexpr ULONGLONG minimumKernelAddress = 0xffff800000000000ULL;

    bool IsCanonicalKernelAddress(_In_ ULONGLONG address)
    {
        return address >= minimumKernelAddress;
    }

    ULONGLONG RotateLeft64(_In_ ULONGLONG value, _In_ unsigned shift)
    {
        shift &= 63;
        return shift == 0 ? value : ((value << shift) | (value >> (64 - shift)));
    }

    ULONGLONG DecodeDebuggerDataQword(
        _In_ ULONGLONG encodedValue,
        _In_ unsigned rotation,
        _In_ ULONGLONG key)
    {
        return _byteswap_uint64(RotateLeft64(encodedValue, rotation)) ^ key;
    }

    bool TryFindKdVersionBlock(
        _In_ GdbSrvController* pController,
        _In_ ADDRESS_TYPE ntBaseAddress,
        _In_ ADDRESS_TYPE cr3,
        _Out_ ADDRESS_TYPE* versionAddress,
        _Out_ KdVersionBlock64* version)
    {
        IMAGE_DOS_HEADER dosHeader = {};
        if (!ReadTargetVirtualMemory(pController, cr3, ntBaseAddress, &dosHeader, sizeof(dosHeader)) ||
            dosHeader.e_magic != IMAGE_DOS_SIGNATURE ||
            dosHeader.e_lfanew <= 0 ||
            dosHeader.e_lfanew >= 0x100000)
        {
            return false;
        }

        IMAGE_NT_HEADERS64 ntHeaders = {};
        const ADDRESS_TYPE ntHeadersAddress = ntBaseAddress + dosHeader.e_lfanew;
        if (!ReadTargetVirtualMemory(pController, cr3, ntHeadersAddress, &ntHeaders, sizeof(ntHeaders)) ||
            ntHeaders.Signature != IMAGE_NT_SIGNATURE ||
            ntHeaders.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            ntHeaders.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            return false;
        }

        const DWORD sectionCount = (std::min)(static_cast<DWORD>(ntHeaders.FileHeader.NumberOfSections), static_cast<DWORD>(32));
        std::vector<IMAGE_SECTION_HEADER> sections(sectionCount);
        const ADDRESS_TYPE sectionHeadersAddress =
            ntHeadersAddress + FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader) + ntHeaders.FileHeader.SizeOfOptionalHeader;
        if (sectionCount == 0 ||
            !ReadTargetVirtualMemory(pController, cr3, sectionHeadersAddress, sections.data(), sections.size() * sizeof(IMAGE_SECTION_HEADER)))
        {
            return false;
        }

        for (const IMAGE_SECTION_HEADER& section : sections)
        {
            static const BYTE dataSectionName[] = { '.', 'd', 'a', 't', 'a', 0 };
            if (memcmp(section.Name, dataSectionName, sizeof(dataSectionName)) != 0)
            {
                continue;
            }

            constexpr DWORD maximumDataSectionSize = 0x02000000;
            const DWORD dataSize = section.Misc.VirtualSize;
            if (dataSize < sizeof(KdVersionBlock64) || dataSize > maximumDataSectionSize)
            {
                return false;
            }

            const ADDRESS_TYPE dataAddress = ntBaseAddress + section.VirtualAddress;
            std::vector<BYTE> data(dataSize);
            if (!ReadTargetVirtualMemory(pController, cr3, dataAddress, data.data(), data.size()))
            {
                AppendVMwareLog("failed to read nt .data address=%I64x size=%x\n",
                    static_cast<ULONGLONG>(dataAddress),
                    dataSize);
                return false;
            }

            for (size_t offset = 0; offset + sizeof(KdVersionBlock64) <= data.size(); offset += sizeof(ULONGLONG))
            {
                KdVersionBlock64 candidate = {};
                memcpy(&candidate, data.data() + offset, sizeof(candidate));
                if (candidate.MajorVersion != kdMajorVersionNt ||
                    candidate.ProtocolVersion != kdProtocolVersion ||
                    candidate.MachineType != IMAGE_FILE_MACHINE_AMD64 ||
                    candidate.KernBase != ntBaseAddress ||
                    !IsCanonicalKernelAddress(candidate.PsLoadedModuleList) ||
                    !IsCanonicalKernelAddress(candidate.DebuggerDataList))
                {
                    continue;
                }

                ListEntry64 debuggerDataList = {};
                if (!ReadTargetVirtualMemory(
                        pController,
                        cr3,
                        candidate.DebuggerDataList,
                        &debuggerDataList,
                        sizeof(debuggerDataList)) ||
                    !IsCanonicalKernelAddress(debuggerDataList.Flink) ||
                    !IsCanonicalKernelAddress(debuggerDataList.Blink))
                {
                    continue;
                }

                *versionAddress = dataAddress + offset;
                *version = candidate;
                AppendVMwareLog(
                    "KdVersionBlock found address=%I64x build=%u kern=%I64x modules=%I64x dataList=%I64x\n",
                    static_cast<ULONGLONG>(*versionAddress),
                    candidate.MinorVersion,
                    candidate.KernBase,
                    candidate.PsLoadedModuleList,
                    candidate.DebuggerDataList);
                return true;
            }

            return false;
        }

        return false;
    }

    bool TryDecodeKdDebuggerData(
        _In_ GdbSrvController* pController,
        _In_ ADDRESS_TYPE cr3,
        _In_ const KdVersionBlock64& version,
        _Out_ ADDRESS_TYPE* debuggerDataAddress,
        _Out_ std::vector<BYTE>* decodedDebuggerData)
    {
        ListEntry64 debuggerDataList = {};
        if (!ReadTargetVirtualMemory(
                pController,
                cr3,
                version.DebuggerDataList,
                &debuggerDataList,
                sizeof(debuggerDataList)) ||
            !IsCanonicalKernelAddress(debuggerDataList.Flink))
        {
            return false;
        }

        constexpr size_t probeSize = sizeof(ULONGLONG) * 4;
        BYTE encodedProbe[probeSize] = {};
        if (!ReadTargetVirtualMemory(
                pController,
                cr3,
                debuggerDataList.Flink,
                encodedProbe,
                sizeof(encodedProbe)))
        {
            return false;
        }

        ULONGLONG encodedQwords[probeSize / sizeof(ULONGLONG)] = {};
        memcpy(encodedQwords, encodedProbe, sizeof(encodedQwords));

        unsigned matchingRotation = 0;
        ULONGLONG matchingKey = 0;
        ULONG debuggerDataSize = 0;
        bool foundEncoding = false;
        for (unsigned rotation = 0; rotation < 64; ++rotation)
        {
            const ULONGLONG key =
                _byteswap_uint64(RotateLeft64(encodedQwords[1], rotation)) ^ version.DebuggerDataList;
            const ULONGLONG decodedTagAndSize = DecodeDebuggerDataQword(encodedQwords[2], rotation, key);
            const ULONG ownerTag = static_cast<ULONG>(decodedTagAndSize);
            const ULONG size = static_cast<ULONG>(decodedTagAndSize >> 32);
            const ULONGLONG kernBase = DecodeDebuggerDataQword(encodedQwords[3], rotation, key);
            if (ownerTag == kdDebuggerDataOwnerTag &&
                size >= sizeof(DebugDataHeader64) &&
                size <= 0x10000 &&
                kernBase == version.KernBase)
            {
                if (foundEncoding)
                {
                    AppendVMwareLog("ambiguous KdDebuggerDataBlock encoding\n");
                    return false;
                }

                matchingRotation = rotation;
                matchingKey = key;
                debuggerDataSize = size;
                foundEncoding = true;
            }
        }

        if (!foundEncoding)
        {
            AppendVMwareLog("failed to derive KdDebuggerDataBlock encoding address=%I64x\n",
                debuggerDataList.Flink);
            return false;
        }

        const size_t encodedSize = (static_cast<size_t>(debuggerDataSize) + sizeof(ULONGLONG) - 1) &
                                   ~(sizeof(ULONGLONG) - 1);
        std::vector<BYTE> encodedData(encodedSize);
        if (!ReadTargetVirtualMemory(
                pController,
                cr3,
                debuggerDataList.Flink,
                encodedData.data(),
                encodedData.size()))
        {
            return false;
        }

        std::vector<BYTE> decodedData(encodedSize);
        for (size_t offset = 0; offset < encodedSize; offset += sizeof(ULONGLONG))
        {
            ULONGLONG encodedValue = 0;
            memcpy(&encodedValue, encodedData.data() + offset, sizeof(encodedValue));
            const ULONGLONG decodedValue = DecodeDebuggerDataQword(encodedValue, matchingRotation, matchingKey);
            memcpy(decodedData.data() + offset, &decodedValue, sizeof(decodedValue));
        }
        decodedData.resize(debuggerDataSize);

        DebugDataHeader64 decodedHeader = {};
        memcpy(&decodedHeader, decodedData.data(), sizeof(decodedHeader));
        ULONGLONG decodedKernBase = 0;
        memcpy(&decodedKernBase, decodedData.data() + sizeof(decodedHeader), sizeof(decodedKernBase));
        if (decodedHeader.Blink != version.DebuggerDataList ||
            decodedHeader.OwnerTag != kdDebuggerDataOwnerTag ||
            decodedHeader.Size != debuggerDataSize ||
            decodedKernBase != version.KernBase)
        {
            return false;
        }

        *debuggerDataAddress = debuggerDataList.Flink;
        *decodedDebuggerData = std::move(decodedData);
        AppendVMwareLog(
            "KdDebuggerDataBlock decoded address=%I64x size=%x rotation=%u key=%I64x\n",
            static_cast<ULONGLONG>(*debuggerDataAddress),
            debuggerDataSize,
            matchingRotation,
            matchingKey);
        return true;
    }

    bool TryLoadWindowsDebuggerData(
        _In_ GdbSrvController* pController,
        _In_ ADDRESS_TYPE ntBaseAddress,
        _In_ ADDRESS_TYPE cr3,
        _Out_ WindowsDebuggerData* debuggerData)
    {
        WindowsDebuggerData result = {};
        if (!TryFindKdVersionBlock(
                pController,
                ntBaseAddress,
                cr3,
                &result.versionAddress,
                &result.version) ||
            !TryDecodeKdDebuggerData(
                pController,
                cr3,
                result.version,
                &result.debuggerDataAddress,
                &result.decodedDebuggerData))
        {
            return false;
        }

        *debuggerData = std::move(result);
        return true;
    }
}

//=============================================================================
// Public function definitions
//=============================================================================

// CLiveExdiGdbSrvServer

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::GetTargetInfo(
    /* [out] */ PGLOBAL_TARGET_INFO_STRUCT pgti)
{
    CheckAndZeroOutArgs(pgti);

    pgti->TargetProcessorFamily = m_detectedProcessorFamily;
    pgti->szProbeName = COMHelpers::CopyStringToTaskMem(L"ExdiGdbServer");
    if (pgti->szProbeName == nullptr)
    {
        return E_OUTOFMEMORY;
    }
    pgti->szTargetName= COMHelpers::CopyStringToTaskMem(L"GdbServer Target");
    if (pgti->szTargetName == nullptr)
    {
        CoTaskMemFree(reinterpret_cast<LPVOID>(pgti->szProbeName));
        return E_OUTOFMEMORY;
    }
    memset(&pgti->dbc, 0, sizeof(pgti->dbc));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::GetRunStatus(
    /* [out] */ PRUN_STATUS_TYPE persCurrent,
    /* [out] */ PHALT_REASON_TYPE pehrCurrent,
    /* [out] */ ADDRESS_TYPE *pCurrentExecAddress,
    /* [out] */ DWORD *pdwExceptionCode,
    /* [out] */ DWORD *pdwProcessorNumberOfLastEvent)
{
    try
    {
        CheckAndZeroOutArgs(persCurrent, pehrCurrent, pCurrentExecAddress, pdwExceptionCode, pdwProcessorNumberOfLastEvent);

        if (m_targetIsRunning)
        {
            *persCurrent = rsRunning;
            *pehrCurrent = hrUnknown;
            *pCurrentExecAddress = 0;
        }
        else
        {
            *persCurrent = rsHalted;
            *pehrCurrent = hrUser;

            if (m_lastResumingCommandWasStep)
            {
                *pehrCurrent = hrStep;
            }

            *pCurrentExecAddress = GetCurrentExecutionAddress(pdwProcessorNumberOfLastEvent);
        }

        *pdwExceptionCode = 0;

        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}


HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::Run(void)
{
    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }
        ClearLastAmd64DataBreakpointHit();
        pController->ResetAsynchronousCmdStopReplyPacket();
        pController->StartRunCommand();

        if (m_pRunNotificationListener != nullptr)
        {
            m_pRunNotificationListener->NotifyRunStateChange(rsRunning, hrUser, 0, 0, 0);
        }

        m_lastResumingCommandWasStep = false;
        pController->SetAsynchronousCmdStopReplyPacket();
        m_targetIsRunning = true;

        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}

//
//  Halt                Interrupt the target
//
//  Request:
//      0x03 character -> Interrupt target character
//      ?              -> Query target halt reason if we don't receive a stop reply packet
//
//  Response:
//
//      T02.....       -> stop reply packet with a signal SIGINT.
//
//  Note:
//  GDB is almost entirely non-preemptive, which is reflected in the sequence of packet exchanges of RSP.
//  The exception is when GDB wishes to interrupt an executing program (via ctrl-break).
//  A single byte, 0x03, is sent (no packet structure). If the target is prepared to handle such interrupts
//  it should recognize such byte. However not all servers are capable of handling such request.
//  The server is free to ignore such out-of-band characters.
//
HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::Halt(void)
{
    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }
        HRESULT hr = E_FAIL;

        if (m_pRunNotificationListener != nullptr)
        {
            DWORD eventProcessor = 0;
            ADDRESS_TYPE currentAddress = m_lastPcAddress;
            bool eventNotification = false;
            if (pController->HandleInterruptTarget(reinterpret_cast<AddressType *>(&currentAddress), &eventProcessor, &eventNotification))
            {
                m_targetIsRunning = false;
                if (currentAddress != 0)
                {
                    m_lastPcAddress = currentAddress;
                }
                if (eventNotification)
                {
                    m_pRunNotificationListener->NotifyRunStateChange(rsHalted, hrUser, currentAddress, 0, eventProcessor);
                }
                hr = S_OK;
            }
            else
            {
                MessageBox(0, _T("The Target break interrupt command failed or the GdbServer does not support the break command."),nullptr, MB_ICONERROR);
                hr = E_NOTIMPL;
            }
        }

        return hr;
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::DoSingleStep(DWORD dwProcessorNumber)
{
    try
    {
        DWORD processorCount;
        HRESULT result = GetNumberOfProcessors(&processorCount);
        if (FAILED(result))
        {
            return result;
        }

        if (dwProcessorNumber >= processorCount)
        {
            return E_INVALIDARG;
        }
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }
        ClearLastAmd64DataBreakpointHit();
        pController->ResetAsynchronousCmdStopReplyPacket();
        pController->StartStepCommand(dwProcessorNumber);

        if (m_pRunNotificationListener != nullptr)
        {
            m_pRunNotificationListener->NotifyRunStateChange(rsRunning, hrUser, 0, 0, dwProcessorNumber);
        }

        m_lastResumingCommandWasStep = true;
        pController->SetAsynchronousCmdStopReplyPacket();
        m_targetIsRunning = true;

        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::Reboot(void)
{
    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }
        //  This should reboot only the target machine.
        if (pController->RestartGdbSrvTarget())
        {
            return S_OK;
        }
        return E_FAIL;
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::GetNbCodeBpAvail(
    /* [out] */ DWORD *pdwNbHwCodeBpAvail,
    /* [out] */ DWORD *pdwNbSwCodeBpAvail)
{
    if (pdwNbHwCodeBpAvail == nullptr || pdwNbSwCodeBpAvail == nullptr)
    {
        return E_POINTER;
    }

    *pdwNbHwCodeBpAvail = *pdwNbSwCodeBpAvail = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::GetNbDataBpAvail(
    /* [out] */ DWORD *pdwNbDataBpAvail)
{
    if (pdwNbDataBpAvail == nullptr)
    {
        return E_POINTER;
    }

    //  We support data breakpoints
    *pdwNbDataBpAvail = 1;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::AddCodeBreakpoint(
    /* [in] */ ADDRESS_TYPE Address,
    /* [in] */ CBP_KIND cbpk,
    /* [in] */ MEM_TYPE mt,
    /* [in] */ DWORD dwExecMode,
    /* [in] */ DWORD dwTotalBypassCount,
    /* [out] */ IeXdiCodeBreakpoint3 **ppieXdiCodeBreakpoint)
{
    UNREFERENCED_PARAMETER(cbpk);
    UNREFERENCED_PARAMETER(dwTotalBypassCount);
    UNREFERENCED_PARAMETER(dwExecMode);

    if (ppieXdiCodeBreakpoint == nullptr)
    {
        return E_POINTER;
    }

    *ppieXdiCodeBreakpoint = nullptr;

    if (mt != mtVirtual)
    {
        return E_INVALIDARG;
    }

    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }
        unsigned breakpointNumber = pController->CreateCodeBreakpoint(Address);

        BasicExdiBreakpoint *pBreakpoint = new CComObject<BasicExdiBreakpoint>();
        pBreakpoint->Initialize(Address, breakpointNumber);
        pBreakpoint->AddRef();
        *ppieXdiCodeBreakpoint = pBreakpoint;
        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::DelCodeBreakpoint(
    /* [in] */ IeXdiCodeBreakpoint3 *pieXdiCodeBreakpoint)
{
    if (pieXdiCodeBreakpoint == nullptr)
    {
        return E_POINTER;
    }

    CComPtr<IBasicExdiBreakpoint> pBreakpoint;
    HRESULT result = pieXdiCodeBreakpoint->QueryInterface(&pBreakpoint);
    if (SUCCEEDED(result))
    {
        ADDRESS_TYPE address = pBreakpoint->GetBreakPointAddress();
        unsigned breakpointNumber = pBreakpoint->GetBreakpointNumber();
        try
        {
            AsynchronousGdbSrvController * pController = GetGdbSrvController();
            if (pController != nullptr)
            {
                pController->DeleteCodeBreakpoint(breakpointNumber, address);
                result = S_OK;
            }
            else
            {
                result = E_POINTER;
            }
        }
        CATCH_AND_RETURN_HRESULT;
    }
    return result;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::AddDataBreakpoint(
    /* [in] */ ADDRESS_TYPE Address,
    /* [in] */ ADDRESS_TYPE AddressMask,
    /* [in] */ DWORD dwData,
    /* [in] */ DWORD dwDataMask,
    /* [in] */ BYTE bAccessWidth,
    /* [in] */ MEM_TYPE mt,
    /* [in] */ BYTE bAddressSpace,
    /* [in] */ DATA_ACCESS_TYPE da,
    /* [in] */ DWORD dwTotalBypassCount,
    /* [out] */ IeXdiDataBreakpoint3 **ppieXdiDataBreakpoint)
{
    //  Note that we do not have a way to to set these parameters with
    //  the GdbServer request commands.
    UNREFERENCED_PARAMETER(AddressMask);
    UNREFERENCED_PARAMETER(dwData);
    UNREFERENCED_PARAMETER(dwDataMask);
    UNREFERENCED_PARAMETER(bAddressSpace);
    UNREFERENCED_PARAMETER(dwTotalBypassCount);

    if (ppieXdiDataBreakpoint == nullptr)
    {
        return E_POINTER;
    }

    *ppieXdiDataBreakpoint = nullptr;

    if (mt != mtVirtual)
    {
        return E_INVALIDARG;
    }

    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }
        constexpr BYTE bitsPerByte = 8;
        if (bAccessWidth == 0 || bAccessWidth % bitsPerByte != 0)
        {
            return E_INVALIDARG;
        }
        const BYTE accessWidthInBytes = bAccessWidth / bitsPerByte;
        unsigned breakpointNumber = pController->CreateDataBreakpoint(Address, accessWidthInBytes, da);

        BasicExdiDataBreakpoint * pBreakpoint = new CComObject<BasicExdiDataBreakpoint>();
        pBreakpoint->Initialize(Address, breakpointNumber, da, bAccessWidth);
        pBreakpoint->AddRef();
        *ppieXdiDataBreakpoint = pBreakpoint;
        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::DelDataBreakpoint(
    /* [in] */ IeXdiDataBreakpoint3 * pieXdiDataBreakpoint)
{
    if (pieXdiDataBreakpoint == nullptr)
    {
        return E_POINTER;
    }

    CComPtr<IBasicExdiDataBreakpoint> pBreakpoint;
    HRESULT result = pieXdiDataBreakpoint->QueryInterface(&pBreakpoint);
    if (SUCCEEDED(result))
    {
        ADDRESS_TYPE address = pBreakpoint->GetBreakPointAddress();
        unsigned breakpointNumber = pBreakpoint->GetBreakpointNumber();
        BYTE accessWidth = pBreakpoint->GetBreakPointAccessWidth();
        DATA_ACCESS_TYPE accessType = pBreakpoint->GetBreakPointAccessType();
        try
        {
            AsynchronousGdbSrvController * pController = GetGdbSrvController();
            if (pController != nullptr)
            {
                constexpr BYTE bitsPerByte = 8;
                if (accessWidth == 0 || accessWidth % bitsPerByte != 0)
                {
                    return E_INVALIDARG;
                }
                pController->DeleteDataBreakpoint(
                    breakpointNumber,
                    address,
                    accessWidth / bitsPerByte,
                    accessType);
                result = S_OK;
            }
            else
            {
                result = E_POINTER;
            }
        }
        CATCH_AND_RETURN_HRESULT;
    }
    return result;
}


HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::StartNotifyingRunChg(
    /* [in] */ IeXdiClientNotifyRunChg3 *pieXdiClientNotifyRunChg,
    /* [out] */ DWORD *pdwConnectionCookie)
{
    if (pieXdiClientNotifyRunChg == nullptr || pdwConnectionCookie == nullptr)
    {
        return E_POINTER;
    }

    *pdwConnectionCookie = s_ConnectionCookie;

    //StartNotifyingRunChg is invoked by COM in STA environment, so no need for a critical section here
    if (m_pRunNotificationListener != nullptr)
    {
        //Theoretically EXDI servers can support more than one run change notification.
        //Practically, debugging engine only uses one and the support for multiple ones will most likely be deprecated.
        return E_FAIL;
    }

    m_pRunNotificationListener = pieXdiClientNotifyRunChg;

    return S_OK;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::StopNotifyingRunChg(
    /* [in] */ DWORD dwConnectionCookie)
{
    if (dwConnectionCookie != s_ConnectionCookie)
    {
        return E_INVALIDARG;
    }

    m_pRunNotificationListener = nullptr;

    return S_OK;
}

static HRESULT SafeArrayFromByteArray(_In_reads_bytes_(arraySize) const char *pByteArray, size_t arraySize, _Out_ SAFEARRAY **pSafeArray)
{
    assert(pByteArray != nullptr && pSafeArray != nullptr);
    ULONG copiedSize = static_cast<ULONG>(arraySize);
    *pSafeArray = SafeArrayCreateVector(VT_UI1, 0, copiedSize);
    if (*pSafeArray == nullptr)
    {
        return E_FAIL;
    }

    memcpy((*pSafeArray)->pvData, pByteArray, copiedSize);

    return S_OK;
}

ADDRESS_TYPE CLiveExdiGdbSrvServer::GetVMwareCr3(_In_ ADDRESS_TYPE virtualAddress)
{
    if (IsCanonicalKernelAddress(virtualAddress) && m_kernelCr3 != 0)
    {
        return m_kernelCr3;
    }

    AsynchronousGdbSrvController* const pController = GetGdbSrvController();
    if (pController == nullptr)
    {
        return 0;
    }

    const unsigned activeProcessor = pController->GetLastKnownActiveCpu();
    if (activeProcessor < m_processorCr3.size())
    {
        return m_processorCr3[activeProcessor];
    }

    return 0;
}

void CLiveExdiGdbSrvServer::OverlayKdDebuggerData(
    _In_ ADDRESS_TYPE address,
    _Inout_updates_bytes_(size) void* data,
    _In_ size_t size) const
{
    if (data == nullptr || size == 0 || m_decodedKdDebuggerData.empty())
    {
        return;
    }

    const ADDRESS_TYPE blockAddress = m_kdDebuggerDataAddress;
    size_t bufferOffset = 0;
    size_t blockOffset = 0;
    if (address < blockAddress)
    {
        const ULONGLONG distance = blockAddress - address;
        if (distance >= size)
        {
            return;
        }
        bufferOffset = static_cast<size_t>(distance);
    }
    else
    {
        const ULONGLONG distance = address - blockAddress;
        if (distance >= m_decodedKdDebuggerData.size())
        {
            return;
        }
        blockOffset = static_cast<size_t>(distance);
    }

    const size_t bytesToCopy = (std::min)(
        size - bufferOffset,
        m_decodedKdDebuggerData.size() - blockOffset);
    memcpy(
        static_cast<BYTE*>(data) + bufferOffset,
        m_decodedKdDebuggerData.data() + blockOffset,
        bytesToCopy);
}

HRESULT CLiveExdiGdbSrvServer::InitializeWindowsDebuggerData(
    _In_ AsynchronousGdbSrvController* pController,
    _In_ ADDRESS_TYPE ntBaseAddress,
    _In_ ADDRESS_TYPE cr3)
{
    if (m_ntBaseAddress == ntBaseAddress &&
        !m_kdVersionBlock.empty() &&
        !m_decodedKdDebuggerData.empty())
    {
        return S_OK;
    }

    WindowsDebuggerData debuggerData = {};
    if (!TryLoadWindowsDebuggerData(pController, ntBaseAddress, cr3, &debuggerData))
    {
        m_ntBaseAddress = 0;
        m_kdVersionBlock.clear();
        m_kdDebuggerDataAddress = 0;
        m_decodedKdDebuggerData.clear();
        AppendVMwareLog("Windows debugger data initialization failed nt=%I64x cr3=%I64x\n",
            static_cast<ULONGLONG>(ntBaseAddress),
            static_cast<ULONGLONG>(cr3));
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    const BYTE* const versionBytes = reinterpret_cast<const BYTE*>(&debuggerData.version);
    m_ntBaseAddress = ntBaseAddress;
    m_kdVersionBlock.assign(versionBytes, versionBytes + sizeof(debuggerData.version));
    m_kdDebuggerDataAddress = debuggerData.debuggerDataAddress;
    m_decodedKdDebuggerData = std::move(debuggerData.decodedDebuggerData);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::ReadVirtualMemory(
    /* [in] */ ADDRESS_TYPE Address,
    /* [in] */ DWORD dwBytesToRead,
    SAFEARRAY * *pbReadBuffer)
{
    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pbReadBuffer == nullptr || pController == nullptr)
        {
            return E_POINTER;
        }
        memoryAccessType memType = {0};
        pController->GetMemoryPacketType(m_lastPSRvalue, &memType);

        if (IsCurrentTargetVMware())
        {
            ADDRESS_TYPE cr3 = GetVMwareCr3(Address);
            if (cr3 == 0)
            {
                CONTEXT_X86_64 currentContext = {};
                unsigned activeProcessor = pController->GetLastKnownActiveCpu();
                if (activeProcessor == C_ALLCORES)
                {
                    activeProcessor = 0;
                }
                HRESULT contextResult = GetContextEx(activeProcessor, &currentContext);
                if (contextResult == S_OK)
                {
                    cr3 = GetVMwareCr3(Address);
                }
            }

            if (cr3 != 0)
            {
                SimpleCharBuffer translatedBuffer;
                if (ReadTargetVirtualMemoryBuffer(pController, cr3, Address, dwBytesToRead, &translatedBuffer))
                {
                    OverlayKdDebuggerData(Address, translatedBuffer.GetInternalBuffer(), translatedBuffer.GetLength());
                    return SafeArrayFromByteArray(translatedBuffer.GetInternalBuffer(), translatedBuffer.GetLength(), pbReadBuffer);
                }

                AppendVMwareLog("ReadVirtualMemory short read va=%I64x size=%x cr3=%I64x\n",
                    static_cast<ULONGLONG>(Address),
                    dwBytesToRead,
                    static_cast<ULONGLONG>(cr3));
                return SafeArrayFromByteArray("", 0, pbReadBuffer);
            }
        }

        SimpleCharBuffer buffer = pController->ReadMemory(Address, dwBytesToRead, memType);
        OverlayKdDebuggerData(Address, buffer.GetInternalBuffer(), buffer.GetLength());
        return SafeArrayFromByteArray(buffer.GetInternalBuffer(), buffer.GetLength(), pbReadBuffer);
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::WriteVirtualMemory(
        /* [in] */ ADDRESS_TYPE Address,
        /* [in] */ SAFEARRAY * pBuffer,
        /* [out] */ DWORD *pdwBytesWritten)
{
    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pBuffer == nullptr || pdwBytesWritten == nullptr || pController == nullptr)
        {
            return E_POINTER;
        }

        if (pBuffer->cDims != 1)
        {
            return E_INVALIDARG;
        }

        VARTYPE dataType;
        if (FAILED(SafeArrayGetVartype(pBuffer, &dataType)) || dataType != VT_UI1)
        {
            return E_INVALIDARG;
        }

        ULONG bufferSize = pBuffer->rgsabound[0].cElements;
        PVOID pRawBuffer = pBuffer->pvData;

        memoryAccessType memType = {0};
        pController->GetMemoryPacketType(m_lastPSRvalue, &memType);

        bool isWriteDone = pController->WriteMemory(Address, bufferSize, pRawBuffer, pdwBytesWritten, memType);
        if (isWriteDone)
        {
            return S_OK;
        }
        return E_FAIL;
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::ReadPhysicalMemoryOrPeriphIO(
        /* [in] */ ADDRESS_TYPE Address,
        /* [in] */ ADDRESS_SPACE_TYPE AddressSpace,
        /* [in] */ DWORD dwBytesToRead,
        /* [out] */ SAFEARRAY * *pReadBuffer)
{
    UNREFERENCED_PARAMETER(AddressSpace);

    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pReadBuffer == nullptr || pController == nullptr)
        {
            return E_POINTER;
        }

        if (IsCurrentTargetVMware())
        {
            SimpleCharBuffer buffer;
            if (!buffer.TryEnsureCapacity(dwBytesToRead))
            {
                return E_OUTOFMEMORY;
            }

            buffer.SetLength(dwBytesToRead);
            if (dwBytesToRead != 0 &&
                !ReadVMwarePhysicalMemory(
                    pController,
                    Address,
                    buffer.GetInternalBuffer(),
                    dwBytesToRead))
            {
                AppendVMwareLog(
                    "ReadPhysicalMemory short read pa=%I64x size=%x\n",
                    static_cast<ULONGLONG>(Address),
                    dwBytesToRead);
                return SafeArrayFromByteArray("", 0, pReadBuffer);
            }

            return SafeArrayFromByteArray(
                buffer.GetInternalBuffer(),
                buffer.GetLength(),
                pReadBuffer);
        }

        memoryAccessType memoryType = {0};
        memoryType.isPhysical = pController->GetPAMemoryMode() ? 0 : 1;

        //
        // Set the PA memory access configuration option for servers that
        // support this feature

        pController->HandleConfigPAMemAccessMode(memoryType, true);
        SimpleCharBuffer buffer = pController->ReadMemory(Address, dwBytesToRead, memoryType);

        //
        // Disable the PA memory access configuration option if it's enabled
        //

        pController->HandleConfigPAMemAccessMode(memoryType, false);
        return SafeArrayFromByteArray(buffer.GetInternalBuffer(), buffer.GetLength(), pReadBuffer);
    }
    CATCH_AND_RETURN_HRESULT;
}


HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::WritePhysicalMemoryOrPeriphIO(
        /* [in] */ ADDRESS_TYPE Address,
        /* [in] */ ADDRESS_SPACE_TYPE AddressSpace,
        /* [in] */ SAFEARRAY * pBuffer,
        /* [out] */ DWORD *pdwBytesWritten)
{
    UNREFERENCED_PARAMETER(AddressSpace);
    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pBuffer == nullptr || pdwBytesWritten == nullptr || pController == nullptr)
        {
            return E_POINTER;
        }

        if (pBuffer->cDims != 1)
        {
            return E_INVALIDARG;
        }

        VARTYPE dataType;
        if (FAILED(SafeArrayGetVartype(pBuffer, &dataType)) || dataType != VT_UI1)
        {
            return E_INVALIDARG;
        }

        ULONG bufferSize = pBuffer->rgsabound[0].cElements;
        PVOID pRawBuffer = pBuffer->pvData;

        memoryAccessType memType = {0};
        memType.isPhysical = 1;
        bool isWriteDone = pController->WriteMemory(Address, bufferSize, pRawBuffer, pdwBytesWritten, memType);
        if (isWriteDone)
        {
            return S_OK;
        }
        return E_FAIL;
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::Ioctl(
        /* [in] */ SAFEARRAY * pInputBuffer,
        /* [in] */ DWORD dwBuffOutSize,
        /* [out] */ SAFEARRAY * *pOutputBuffer)
{
    AsynchronousGdbSrvController * pController = GetGdbSrvController();
    if (pOutputBuffer == nullptr || pController == nullptr)
    {
        return E_POINTER;
    }

    HRESULT hr = E_INVALIDARG;
    VARTYPE dataType;
    if (FAILED(SafeArrayGetVartype(pInputBuffer, &dataType)) || dataType != VT_UI1)
    {
        return hr;
    }

    PVOID pRawBuffer = pInputBuffer->pvData;
    if (pRawBuffer == nullptr)
    {
        return hr;
    }

    try
    {
        ULONG bufferSize = pInputBuffer->rgsabound[0].cElements;
        const DBGENG_EXDI_IOCTL_CODE_V3_EX * pExdiV3 = reinterpret_cast<const DBGENG_EXDI_IOCTL_CODE_V3_EX *>(pRawBuffer);
        DBGENG_EXDI_IOCTL_CODE_V3_EX ioctlCode = *pExdiV3;
        switch(ioctlCode)
        {
            //  Get additional gdb server info that will be used by the debugger engine
            case DBGENG_EXDI_IOCTL_V3_GET_ADDITIONAL_SERVER_INFO:
            {
                if (bufferSize == sizeof(DBGENG_EXDI_IOCTL_V3_GET_ADDITIONAL_SERVER_INFO_EX_IN))
                {
                    const DBGENG_EXDI_IOCTL_V3_GET_ADDITIONAL_SERVER_INFO_EX_IN * pAdditionalInfo =
                        reinterpret_cast<const DBGENG_EXDI_IOCTL_V3_GET_ADDITIONAL_SERVER_INFO_EX_IN *>(pRawBuffer);
                    if (pAdditionalInfo -> request.HeuristicChunkSize)
                    {
                        size_t bytesToCopy = min(dwBuffOutSize, sizeof(m_heuristicChunkSize));
                        hr = SafeArrayFromByteArray(reinterpret_cast<const char *>(&m_heuristicChunkSize), bytesToCopy, pOutputBuffer);
                    }
                    else if (pAdditionalInfo->request.RequireMemoryAccessByPA)
                    {
                        size_t bytesToCopy = min(dwBuffOutSize, sizeof(m_RequireMemoryAccessByPA));
                        hr = SafeArrayFromByteArray(reinterpret_cast<const char*>(&m_RequireMemoryAccessByPA), bytesToCopy, pOutputBuffer);
                    }
                    else
                    {
                        hr = E_NOTIMPL;
                    }

                }
            }
            break;

            //  Store the KPCR value
            case DBGENG_EXDI_IOCTL_V3_STORE_KPCR_VALUE:
            {
                if (bufferSize == sizeof(DBGENG_EXDI_IOCTL_STORE_KPCR_V3_EX_IN))
                {
                    const DBGENG_EXDI_IOCTL_STORE_KPCR_V3_EX_IN * pKPCRV3 = reinterpret_cast<const DBGENG_EXDI_IOCTL_STORE_KPCR_V3_EX_IN *>(pRawBuffer);
                    //  Extract the processor number
                    ULONG processorNumber = pKPCRV3->processorNumber;
                    //  Extract the processor block array offset
                    ULONG64 kpcrOffset = pKPCRV3->kpcrOffset;
                    if (kpcrOffset != 0)
                    {
                        pController->SetKpcrOffset(processorNumber, kpcrOffset);
                        size_t bytesToCopy = min(dwBuffOutSize, sizeof(kpcrOffset));
                        hr = SafeArrayFromByteArray(reinterpret_cast<const char *>(&kpcrOffset), bytesToCopy, pOutputBuffer);
                    }
                }
            }
            break;

            // Locate ntoskrnl through the IDT when dbgeng asks the EXDI server for NT base.
            case DBGENG_EXDI_IOCTL_V3_GET_NT_BASE_ADDRESS_VALUE:
            {
                CONTEXT_X86_64 currentContext = {};
                ADDRESS_TYPE ntBaseAddress = 0;
                hr = GetContextEx(0, &currentContext);
                if (hr == S_OK)
                {
                    hr = FindNtBaseAddressFromContext(pController, currentContext, &ntBaseAddress);
                    if (hr == S_OK)
                    {
                        (void)InitializeWindowsDebuggerData(pController, ntBaseAddress, currentContext.RegCr3);
                        hr = SafeArrayFromByteArray(reinterpret_cast<const char*>(&ntBaseAddress), sizeof(ntBaseAddress), pOutputBuffer);
                    }
                }
            }
            break;

            //  Read the special registers content Architecture specific.
            case DBGENG_EXDI_IOCTL_V3_GET_SPECIAL_REGISTER_VALUE:
            {
                if (bufferSize == sizeof(DBGENG_EXDI_IOCTL_READ_SPECIAL_MEMORY_EX_IN))
                {
                    DBGENG_EXDI_IOCTL_READ_SPECIAL_MEMORY_EX_IN* const pSpecialRegs = reinterpret_cast<DBGENG_EXDI_IOCTL_READ_SPECIAL_MEMORY_EX_IN* const>(pRawBuffer);
                    memoryAccessType memoryType = { 0 };
                    memoryType.isSpecialRegs = 1;
                    SimpleCharBuffer buffer = pController->ReadSystemRegisters(pSpecialRegs->address, pSpecialRegs->bytesToRead, memoryType);
                    hr = SafeArrayFromByteArray(buffer.GetInternalBuffer(), buffer.GetLength(), pOutputBuffer);
                }
            }
            break;

            //  Read the special memory content Architecture specific.
            case DBGENG_EXDI_IOCTL_V3_GET_SUPERVISOR_MODE_MEM_VALUE:
            case DBGENG_EXDI_IOCTL_V3_GET_HYPERVISOR_MODE_MEM_VALUE:
            {
                if (bufferSize == sizeof(DBGENG_EXDI_IOCTL_READ_SPECIAL_MEMORY_EX_IN))
                {
                    DBGENG_EXDI_IOCTL_READ_SPECIAL_MEMORY_EX_IN * const pSpecialRegs = reinterpret_cast<DBGENG_EXDI_IOCTL_READ_SPECIAL_MEMORY_EX_IN * const>(pRawBuffer);
                    memoryAccessType memoryType = {0};
                    if (ioctlCode == DBGENG_EXDI_IOCTL_V3_GET_HYPERVISOR_MODE_MEM_VALUE)
                    {
                        memoryType.isHypervisor = 1;
                    }
                    else
                    {
                        memoryType.isSupervisor = 1;
                    }

                    if (IsCurrentTargetVMware())
                    {
                        ADDRESS_TYPE cr3 = GetVMwareCr3(pSpecialRegs->address);
                        if (cr3 == 0)
                        {
                            CONTEXT_X86_64 currentContext = {};
                            unsigned activeProcessor = pController->GetLastKnownActiveCpu();
                            if (activeProcessor == C_ALLCORES)
                            {
                                activeProcessor = 0;
                            }
                            HRESULT contextResult = GetContextEx(activeProcessor, &currentContext);
                            if (contextResult == S_OK)
                            {
                                cr3 = GetVMwareCr3(pSpecialRegs->address);
                            }
                        }

                        if (cr3 != 0)
                        {
                            SimpleCharBuffer translatedBuffer;
                            if (ReadTargetVirtualMemoryBuffer(pController, cr3, pSpecialRegs->address, pSpecialRegs->bytesToRead, &translatedBuffer))
                            {
                                OverlayKdDebuggerData(
                                    pSpecialRegs->address,
                                    translatedBuffer.GetInternalBuffer(),
                                    translatedBuffer.GetLength());
                                hr = SafeArrayFromByteArray(translatedBuffer.GetInternalBuffer(), translatedBuffer.GetLength(), pOutputBuffer);
                                break;
                            }

                            AppendVMwareLog("special memory short read va=%I64x size=%x cr3=%I64x code=%d\n",
                                static_cast<ULONGLONG>(pSpecialRegs->address),
                                pSpecialRegs->bytesToRead,
                                static_cast<ULONGLONG>(cr3),
                                static_cast<int>(ioctlCode));
                            hr = SafeArrayFromByteArray("", 0, pOutputBuffer);
                            break;
                        }
                    }

                    SimpleCharBuffer buffer = pController->ReadMemory(pSpecialRegs->address, pSpecialRegs->bytesToRead, memoryType);
                    hr = SafeArrayFromByteArray(buffer.GetInternalBuffer(), buffer.GetLength(), pOutputBuffer);
                }
            }
            break;

            default:
            {
                hr = E_NOTIMPL;
            }
        }
        return hr;
    }
    CATCH_AND_RETURN_HRESULT;
}

// DbgEng's Default data-breakpoint mode programs AMD64 DR state through the
// context interface. Keep that state in the EXDI server and realize it with
// GDB watchpoints so the guest debug registers remain untouched.
void CLiveExdiGdbSrvServer::ClearLastAmd64DataBreakpointHit()
{
    m_lastHitAmd64DataBreakpoint = {};
    m_lastHitAmd64DebugRegisterNumber = static_cast<unsigned>(m_virtualAmd64DataBreakpoints.size());
    m_lastHitAmd64ProcessorNumber = (std::numeric_limits<DWORD>::max)();
}

bool CLiveExdiGdbSrvServer::RecordAmd64DataBreakpointHit(
    _In_ ADDRESS_TYPE reportedAddress,
    _In_ DWORD processorNumber)
{
    ClearLastAmd64DataBreakpointHit();

    unsigned matchedRegister = static_cast<unsigned>(m_virtualAmd64DataBreakpoints.size());
    for (unsigned debugRegister = 0;
         debugRegister < m_virtualAmd64DataBreakpoints.size();
         ++debugRegister)
    {
        const VirtualAmd64DataBreakpoint& breakpoint = m_virtualAmd64DataBreakpoints[debugRegister];
        if (!breakpoint.active)
        {
            continue;
        }

        const bool exactMatch = breakpoint.address == reportedAddress;
        const bool vmwareTruncatedMatch =
            IsCurrentTargetVMware() &&
            reportedAddress <= (std::numeric_limits<ULONG>::max)() &&
            static_cast<ULONG>(breakpoint.address) == static_cast<ULONG>(reportedAddress);
        if (!exactMatch && !vmwareTruncatedMatch)
        {
            continue;
        }

        if (matchedRegister < m_virtualAmd64DataBreakpoints.size())
        {
            AppendVMwareLog(
                "virtual-dr ambiguous watchpoint hit reported=%I64x\n",
                static_cast<ULONGLONG>(reportedAddress));
            return false;
        }

        matchedRegister = debugRegister;
    }

    if (matchedRegister >= m_virtualAmd64DataBreakpoints.size())
    {
        AppendVMwareLog(
            "virtual-dr unmatched watchpoint hit reported=%I64x\n",
            static_cast<ULONGLONG>(reportedAddress));
        return false;
    }

    m_lastHitAmd64DataBreakpoint = m_virtualAmd64DataBreakpoints[matchedRegister];
    m_lastHitAmd64DebugRegisterNumber = matchedRegister;
    m_lastHitAmd64ProcessorNumber = processorNumber;
    return true;
}

void CLiveExdiGdbSrvServer::PopulateAmd64DebugRegisters(
    _In_ DWORD processorNumber,
    _Out_ PCONTEXT_X86_64 pContext) const
{
    assert(pContext != nullptr);

    const auto populateBreakpoint = [pContext](
        unsigned debugRegister,
        const VirtualAmd64DataBreakpoint& breakpoint)
    {
        if (!breakpoint.active || debugRegister >= 4)
        {
            return;
        }

        const DWORD64 accessBits = breakpoint.accessType == daWrite ? 1 : 3;
        DWORD64 lengthBits = 0;
        switch (breakpoint.accessWidth)
        {
        case 1:
            lengthBits = 0;
            break;
        case 2:
            lengthBits = 1;
            break;
        case 4:
            lengthBits = 3;
            break;
        case 8:
            lengthBits = 2;
            break;
        default:
            return;
        }

        switch (debugRegister)
        {
        case 0:
            pContext->Dr0 = breakpoint.address;
            break;
        case 1:
            pContext->Dr1 = breakpoint.address;
            break;
        case 2:
            pContext->Dr2 = breakpoint.address;
            break;
        case 3:
            pContext->Dr3 = breakpoint.address;
            break;
        }

        const unsigned controlShift = 16 + (debugRegister * 4);
        pContext->Dr7 |=
            (1ULL << (debugRegister * 2)) |
            ((accessBits | (lengthBits << 2)) << controlShift);
    };

    for (unsigned debugRegister = 0;
         debugRegister < m_virtualAmd64DataBreakpoints.size();
         ++debugRegister)
    {
        populateBreakpoint(debugRegister, m_virtualAmd64DataBreakpoints[debugRegister]);
    }

    if (processorNumber == m_lastHitAmd64ProcessorNumber &&
        m_lastHitAmd64DebugRegisterNumber < m_virtualAmd64DataBreakpoints.size())
    {
        populateBreakpoint(
            m_lastHitAmd64DebugRegisterNumber,
            m_lastHitAmd64DataBreakpoint);
        pContext->Dr6 = 1ULL << m_lastHitAmd64DebugRegisterNumber;
    }
}

void CLiveExdiGdbSrvServer::SynchronizeAmd64DebugRegisters(
    _In_ const CONTEXT_X86_64& context,
    _In_ AsynchronousGdbSrvController* pController)
{
    assert(pController != nullptr);

    const ADDRESS_TYPE addresses[] =
    {
        static_cast<ADDRESS_TYPE>(context.Dr0),
        static_cast<ADDRESS_TYPE>(context.Dr1),
        static_cast<ADDRESS_TYPE>(context.Dr2),
        static_cast<ADDRESS_TYPE>(context.Dr3)
    };
    std::array<VirtualAmd64DataBreakpoint, 4> desiredBreakpoints{};

    for (unsigned debugRegister = 0; debugRegister < desiredBreakpoints.size(); ++debugRegister)
    {
        const DWORD64 enableMask = 3ULL << (debugRegister * 2);
        if ((context.Dr7 & enableMask) == 0)
        {
            continue;
        }

        const DWORD64 control = (context.Dr7 >> (16 + debugRegister * 4)) & 0xf;
        DATA_ACCESS_TYPE accessType;
        switch (control & 3)
        {
        case 1:
            accessType = daWrite;
            break;
        case 3:
            accessType = daBoth;
            break;
        default:
            throw _com_error(E_INVALIDARG);
        }

        BYTE accessWidth;
        switch ((control >> 2) & 3)
        {
        case 0:
            accessWidth = 1;
            break;
        case 1:
            accessWidth = 2;
            break;
        case 2:
            accessWidth = 8;
            break;
        default:
            accessWidth = 4;
            break;
        }

        if (addresses[debugRegister] % accessWidth != 0)
        {
            throw _com_error(E_INVALIDARG);
        }

        desiredBreakpoints[debugRegister] =
            {true, 0, addresses[debugRegister], accessWidth, accessType};
    }

    const auto hasSameConfiguration = [](
        const VirtualAmd64DataBreakpoint& left,
        const VirtualAmd64DataBreakpoint& right)
    {
        return left.active == right.active &&
            left.address == right.address &&
            left.accessWidth == right.accessWidth &&
            left.accessType == right.accessType;
    };

    for (unsigned debugRegister = 0;
         debugRegister < m_virtualAmd64DataBreakpoints.size();
         ++debugRegister)
    {
        VirtualAmd64DataBreakpoint& current = m_virtualAmd64DataBreakpoints[debugRegister];
        const VirtualAmd64DataBreakpoint& desired = desiredBreakpoints[debugRegister];
        if (current.active && !hasSameConfiguration(current, desired))
        {
            pController->DeleteDataBreakpoint(
                current.controllerBreakpointNumber,
                current.address,
                current.accessWidth,
                current.accessType);
            current = {};
        }
    }

    for (unsigned debugRegister = 0;
         debugRegister < m_virtualAmd64DataBreakpoints.size();
         ++debugRegister)
    {
        VirtualAmd64DataBreakpoint& current = m_virtualAmd64DataBreakpoints[debugRegister];
        const VirtualAmd64DataBreakpoint& desired = desiredBreakpoints[debugRegister];
        if (!current.active && desired.active)
        {
            const unsigned controllerBreakpointNumber = pController->CreateDataBreakpoint(
                desired.address,
                desired.accessWidth,
                desired.accessType);
            current = desired;
            current.controllerBreakpointNumber = controllerBreakpointNumber;
        }
    }
}

void CLiveExdiGdbSrvServer::RemoveAllAmd64DataBreakpoints()
{
    AsynchronousGdbSrvController* pController = GetGdbSrvController();
    if (pController == nullptr)
    {
        return;
    }

    for (VirtualAmd64DataBreakpoint& breakpoint : m_virtualAmd64DataBreakpoints)
    {
        if (!breakpoint.active)
        {
            continue;
        }

        try
        {
            pController->DeleteDataBreakpoint(
                breakpoint.controllerBreakpointNumber,
                breakpoint.address,
                breakpoint.accessWidth,
                breakpoint.accessType);
            breakpoint = {};
        }
        catch (...)
        {
            AppendVMwareLog(
                "virtual-dr shutdown cleanup failed address=%I64x width=%u access=%u\n",
                static_cast<ULONGLONG>(breakpoint.address),
                breakpoint.accessWidth,
                static_cast<unsigned>(breakpoint.accessType));
        }
    }

    ClearLastAmd64DataBreakpointHit();
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::GetLastHitBreakpoint(
    /* [out] */ DBGENG_EXDI3_GET_BREAKPOINT_HIT_OUT *pBreakpointInformation)
{
    UNREFERENCED_PARAMETER(pBreakpointInformation);
    //  The current dbgeng.dll Exdi target does not use this function for Intel targets.
    //  Also, there is no a debugger command that calls this function.
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::GetKPCRForProcessor(
    /* [in] */ DWORD dwProcessorNumber,
    /* [out] */ ULONG64 *pKPCRPointer)
{
    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pKPCRPointer == nullptr || pController == nullptr)
        {
            return E_POINTER;
        }
        DWORD totalProcessors = 0;
        HRESULT result = GetNumberOfProcessors(&totalProcessors);
        if (FAILED(result))
        {
            return result;
        }
        if (dwProcessorNumber >= totalProcessors)
        {
            return E_INVALIDARG;
        }
        *pKPCRPointer = pController->GetKpcrOffset(dwProcessorNumber);
        if (*pKPCRPointer == 0)
        {
            return E_NOTIMPL;
        }
        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::ReadKdVersionBlock(
        /* [in] */ DWORD dwBufferSize,
        /* [out] */ SAFEARRAY * *pKdVersionBlockBuffer)
{
    if (pKdVersionBlockBuffer == nullptr)
    {
        return E_POINTER;
    }
    if (dwBufferSize == 0)
    {
        return E_INVALIDARG;
    }
    if (m_kdVersionBlock.empty())
    {
        AsynchronousGdbSrvController* const pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }

        CONTEXT_X86_64 currentContext = {};
        HRESULT result = GetContextEx(0, &currentContext);
        if (FAILED(result))
        {
            return result;
        }

        ADDRESS_TYPE ntBaseAddress = 0;
        result = FindNtBaseAddressFromContext(pController, currentContext, &ntBaseAddress);
        if (FAILED(result))
        {
            return result;
        }

        result = InitializeWindowsDebuggerData(pController, ntBaseAddress, currentContext.RegCr3);
        if (FAILED(result))
        {
            return result;
        }
    }

    const size_t bytesToCopy = (std::min)(
        static_cast<size_t>(dwBufferSize),
        m_kdVersionBlock.size());
    return SafeArrayFromByteArray(
        reinterpret_cast<const char*>(m_kdVersionBlock.data()),
        bytesToCopy,
        pKdVersionBlockBuffer);
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::ReadMSR(
    /* [in] */ DWORD dwProcessorNumber,
    /* [in] */ DWORD dwRegisterIndex,
    /* [out] */ ULONG64 *pValue)
{
    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pValue == nullptr || pController == nullptr)
        {
            return E_POINTER;
        }

        return pController->ReadMsrRegister(dwProcessorNumber, dwRegisterIndex, pValue);
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::WriteMSR(
    /* [in] */ DWORD dwProcessorNumber,
    /* [in] */ DWORD dwRegisterIndex,
    /* [in] */ ULONG64 value)
{
    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }

        return pController->WriteMsrRegister(dwProcessorNumber, dwRegisterIndex, value);
    }
    CATCH_AND_RETURN_HRESULT;
}


// ------------------------------------------------------------------------------


HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::GetContext(
    /* [in] */ DWORD dwProcessorNumber,
    /* [out][in] */ PCONTEXT_ARM4 pContext)
{
    return GetContextEx(dwProcessorNumber, pContext);
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::SetContext(
    /* [in] */ DWORD dwProcessorNumber,
    /* [in] */ CONTEXT_ARM4 Context)
{
    return SetContextEx(dwProcessorNumber, &Context);
}

// ------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::GetContext(
    /* [in] */ DWORD dwProcessorNumber,
    /* [out][in] */ PCONTEXT_X86_64 pContext)
{
    return GetContextEx(dwProcessorNumber, pContext);
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::SetContext(
    /* [in] */ DWORD dwProcessorNumber,
    /* [in] */ CONTEXT_X86_64 Context)
{
    return SetContextEx(dwProcessorNumber, &Context);
}

// ------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::GetContext(
    /* [in] */ DWORD dwProcessorNumber,
    /* [out][in] */ PCONTEXT_X86_EX pContext)
{
    return GetContextEx(dwProcessorNumber, pContext);
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::SetContext(
    /* [in] */ DWORD dwProcessorNumber,
    /* [in] */ CONTEXT_X86_EX Context)
{
    return SetContextEx(dwProcessorNumber, &Context);
}

// ------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::GetContext(
    /* [in] */ DWORD dwProcessorNumber,
    /* [out][in] */ PCONTEXT_ARMV8ARCH64 pContext)
{
    return GetContextEx(dwProcessorNumber, pContext);
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::SetContext(
    /* [in] */ DWORD dwProcessorNumber,
    /* [in] */ CONTEXT_ARMV8ARCH64 context)
{
    return SetContextEx(dwProcessorNumber, &context);
}

// ------------------------------------------------------------------------------

HRESULT CLiveExdiGdbSrvServer::FinalConstruct()
{
    if (SetGdbServerParameters() != S_OK)
    {
        return E_ABORT;
    }

    AsynchronousGdbSrvController * pController = GetGdbSrvController();
    if (pController == nullptr)
    {
        return E_POINTER;
    }
    //  Execute the connection to the GdbServer
    if (SetGdbServerConnection() != S_OK)
    {
        return E_FAIL;
    }
    m_timerId = SetTimer(nullptr, 0, 100, TimerCallback);
    if (m_timerId == 0)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    g_timerOwners.emplace(m_timerId, this);

    return S_OK;
}

void CLiveExdiGdbSrvServer::FinalRelease()
{
    if (m_timerId != 0)
    {
        KillTimer(nullptr, m_timerId);
        g_timerOwners.erase(m_timerId);
        m_timerId = 0;
    }

    AsynchronousGdbSrvController* pController = GetGdbSrvController();
    if (pController != nullptr && m_targetIsRunning)
    {
        AddressType currentAddress = static_cast<AddressType>(m_lastPcAddress);
        DWORD eventProcessor = 0;
        bool eventNotification = false;
        if (pController->HandleInterruptTarget(
                &currentAddress,
                &eventProcessor,
                &eventNotification) &&
            eventNotification)
        {
            m_targetIsRunning = false;
        }
        else
        {
            AppendVMwareLog("virtual-dr shutdown could not halt target\n");
        }
    }

    if (!m_targetIsRunning)
    {
        RemoveAllAmd64DataBreakpoints();
    }
    delete m_pGdbSrvController;
    m_pGdbSrvController = nullptr;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::GetNumberOfProcessors(
    /* [out] */ DWORD *pdwNumberOfProcessors)
{
    if (pdwNumberOfProcessors == nullptr)
    {
        return E_POINTER;
    }
    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }

        *pdwNumberOfProcessors = pController->GetProcessorCount();

        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::GetContextEx(_In_ DWORD processorNumber, _Inout_ PCONTEXT_ARM4 pContext)
{
    if (pContext == nullptr)
    {
        return E_POINTER;
    }

    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }
        pController->StopTargetAtRun();
        memset(pContext, 0, sizeof(CONTEXT_ARM4));

        std::map<std::string, std::string> registers = pController->QueryAllRegisters(processorNumber);
        pContext->R0 = GdbSrvController::ParseRegisterValue32(registers["r0"]);
        pContext->R1 = GdbSrvController::ParseRegisterValue32(registers["r1"]);
        pContext->R2 = GdbSrvController::ParseRegisterValue32(registers["r2"]);
        pContext->R3 = GdbSrvController::ParseRegisterValue32(registers["r3"]);
        pContext->R4 = GdbSrvController::ParseRegisterValue32(registers["r4"]);
        pContext->R5 = GdbSrvController::ParseRegisterValue32(registers["r5"]);
        pContext->R6 = GdbSrvController::ParseRegisterValue32(registers["r6"]);
        pContext->R7 = GdbSrvController::ParseRegisterValue32(registers["r7"]);
        pContext->R8 = GdbSrvController::ParseRegisterValue32(registers["r8"]);
        pContext->R9  = GdbSrvController::ParseRegisterValue32(registers["r9"]);
        pContext->R10  = GdbSrvController::ParseRegisterValue32(registers["r10"]);
        pContext->R11 = GdbSrvController::ParseRegisterValue32(registers["r11"]);
        pContext->R12 = GdbSrvController::ParseRegisterValue32(registers["r12"]);
        pContext->Sp = GdbSrvController::ParseRegisterValue32(registers["sp"]);
        pContext->Lr = GdbSrvController::ParseRegisterValue32(registers["lr"]);
        pContext->Pc = GdbSrvController::ParseRegisterValue32(registers["pc"]);
        pContext->Psr = GdbSrvController::ParseRegisterValue32(registers["Cpsr"]);
        pContext->RegGroupSelection.fControlRegs = TRUE;
        pContext->RegGroupSelection.fIntegerRegs = TRUE;
        // Store the last 'pc' value in order to notify the engine with the last obtained 'pc' value,
        // This is required for cases when the GdbServer responds with target unvailable packet.
        m_lastPcAddress = pContext->Pc;
        m_lastPSRvalue = pContext->Psr;

        //  Get Neon registers
        try
        {
            //  Get Neon registers, if possible
            GetNeonRegisters(pController, registers, pContext);
        }
        catch (...)
        {
            // ignore failure, and don't report Neon registers
            // (this occurs on QEMU, where is not defined the right register mappings)
        }

        if (pContext->RegGroupSelection.fFloatingPointRegs)
        {
            try
            {
                pContext->Fpscr = GdbSrvController::ParseRegisterValue32(registers["Fpscr"]);
            }
            catch (...)
            {
                // no fpscr was found in the returned context (e.g. on QEMU)
                // rather than failing outright, return the still-useful integer context
                pContext->RegGroupSelection.fFloatingPointRegs = FALSE;
            }
        }
        pContext->RegGroupSelection.fDebugRegs = FALSE;

        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::SetContextEx(_In_ DWORD processorNumber, _In_ const CONTEXT_ARM4 *pContext)
{
    if (pContext == nullptr)
    {
        return E_POINTER;
    }

    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }
        pController->StopTargetAtRun();
        std::map<std::string, ULONGLONG> registers;
        if (pContext->RegGroupSelection.fIntegerRegs)
        {
            registers["r0"] = pContext->R0;
            registers["r1"] = pContext->R1;
            registers["r2"] = pContext->R2;
            registers["r3"] = pContext->R3;
            registers["r4"] = pContext->R4;
            registers["r5"] = pContext->R5;
            registers["r6"] = pContext->R6;
            registers["r7"] = pContext->R7;
            registers["r8"] = pContext->R8;
            registers["r9"] = pContext->R9;
            registers["r10"] = pContext->R10;
            registers["r11"] = pContext->R11;
            registers["r12"] = pContext->R12;
            registers["sp"] = pContext->Sp;
            registers["lr"] = pContext->Lr;
            registers["pc"] = pContext->Pc;
            m_lastPcAddress  = pContext->Pc;
            registers["Cpsr"] = pContext->Psr;
        }
        pController->SetRegisters(processorNumber, registers, false);
        if (pContext->RegGroupSelection.fFloatingPointRegs)
        {
            SetNeonRegisters(processorNumber, pContext, pController);
            registers["Fpscr"] = pContext->Fpscr;
        }

        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}

// ------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::GetContextEx(_In_ DWORD processorNumber, _Inout_ PCONTEXT_X86_64 pContext)
{
    if (pContext == nullptr)
    {
        return E_POINTER;
    }

    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }
        pController->StopTargetAtRun();
        memset(pContext, 0, sizeof(CONTEXT_X86_64));

        //We do not fetch the actual descriptors, thus we mark them as invalid
        pContext->DescriptorCs.SegFlags = static_cast<DWORD>(-1);
        pContext->DescriptorSs.SegFlags = static_cast<DWORD>(-1);
        pContext->DescriptorGs.SegFlags = static_cast<DWORD>(-1);
        pContext->DescriptorFs.SegFlags = static_cast<DWORD>(-1);
        pContext->DescriptorEs.SegFlags = static_cast<DWORD>(-1);
        pContext->DescriptorDs.SegFlags = static_cast<DWORD>(-1);

        std::map<std::string, std::string> registers = pController->QueryAllRegisters(processorNumber);
        if (IsCurrentTargetVMware())
        {
            QueryVMwareSpecialRegisters(pController, registers);
        }
        pContext->Rax = GdbSrvController::ParseRegisterValue(registers["rax"]);
        pContext->Rbx = GdbSrvController::ParseRegisterValue(registers["rbx"]);
        pContext->Rcx = GdbSrvController::ParseRegisterValue(registers["rcx"]);
        pContext->Rdx = GdbSrvController::ParseRegisterValue(registers["rdx"]);
        pContext->Rsi = GdbSrvController::ParseRegisterValue(registers["rsi"]);
        pContext->Rdi = GdbSrvController::ParseRegisterValue(registers["rdi"]);
        pContext->Rip = GdbSrvController::ParseRegisterValue(registers["rip"]);
        // Store the last 'pc' value in order to notify the engine with the last obtained 'pc' value,
        // This is required for cases when the GdbServer responds with target unvailable packet.
        m_lastPcAddress = pContext->Rip;
        pContext->Rsp = GdbSrvController::ParseRegisterValue(registers["rsp"]);
        pContext->Rbp = GdbSrvController::ParseRegisterValue(registers["rbp"]);
        pContext->R8  = GdbSrvController::ParseRegisterValue(registers["r8"]);
        pContext->R9  = GdbSrvController::ParseRegisterValue(registers["r9"]);
        pContext->R10 = GdbSrvController::ParseRegisterValue(registers["r10"]);
        pContext->R11 = GdbSrvController::ParseRegisterValue(registers["r11"]);
        pContext->R12 = GdbSrvController::ParseRegisterValue(registers["r12"]);
        pContext->R13 = GdbSrvController::ParseRegisterValue(registers["r13"]);
        pContext->R14 = GdbSrvController::ParseRegisterValue(registers["r14"]);
        pContext->R15 = GdbSrvController::ParseRegisterValue(registers["r15"]);
        if (registers.find("eflags") != registers.end())
        {
            pContext->EFlags = GdbSrvController::ParseRegisterValue32(registers["eflags"]);
        }
        else if (registers.find("rflags") != registers.end())
        {
            pContext->EFlags = GdbSrvController::ParseRegisterValue(registers["rflags"]);
        }
        pContext->RegGroupSelection.fIntegerRegs = TRUE;

        pContext->ModeFlags = AMD64_CONTEXT_AMD64 | AMD64_CONTEXT_CONTROL |
                              AMD64_CONTEXT_INTEGER | AMD64_CONTEXT_SEGMENTS |
                              AMD64_CONTEXT_DEBUG_REGISTERS;

        //  Segment registers
        pContext->SegCs = static_cast<DWORD>(GdbSrvController::ParseRegisterValue(registers["cs"]));
        pContext->SegSs = static_cast<DWORD>(GdbSrvController::ParseRegisterValue(registers["ss"]));
        pContext->SegDs = static_cast<DWORD>(GdbSrvController::ParseRegisterValue(registers["ds"]));
        pContext->SegEs = static_cast<DWORD>(GdbSrvController::ParseRegisterValue(registers["es"]));
        pContext->SegFs = static_cast<DWORD>(GdbSrvController::ParseRegisterValue(registers["fs"]));
        pContext->SegGs = static_cast<DWORD>(GdbSrvController::ParseRegisterValue(registers["gs"]));
        pContext->RegGroupSelection.fSegmentRegs = TRUE;

        //  Control registers (System registers)
        std::map<std::string, std::string> ::const_iterator it = registers.find("cr0");
        if (it != registers.end())
        {
            pContext->RegCr0 = GdbSrvController::ParseRegisterValue(registers["cr0"]);
            pContext->RegCr2 = GdbSrvController::ParseRegisterValue(registers["cr2"]);
            pContext->RegCr3 = GdbSrvController::ParseRegisterValue(registers["cr3"]);
            pContext->RegCr4 = GdbSrvController::ParseRegisterValue(registers["cr4"]);
            pContext->RegCr8 = GdbSrvController::ParseRegisterValue(registers["cr8"]);
            pContext->RegGroupSelection.fSystemRegisters = TRUE;
            constexpr DWORD maximumTrackedProcessors = 256;
            if (processorNumber < maximumTrackedProcessors)
            {
                if (m_processorCr3.size() <= processorNumber)
                {
                    m_processorCr3.resize(processorNumber + 1);
                }
                m_processorCr3[processorNumber] = pContext->RegCr3;
                if (m_amd64SystemRegisters.size() <= processorNumber)
                {
                    m_amd64SystemRegisters.resize(processorNumber + 1);
                }
                m_amd64SystemRegisters[processorNumber] =
                {
                    true,
                    pContext->RegCr0,
                    pContext->RegCr2,
                    pContext->RegCr3,
                    pContext->RegCr4,
                    pContext->RegCr8
                };
            }
            if ((pContext->SegCs & 3) == 0 && pContext->RegCr3 != 0)
            {
                m_kernelCr3 = pContext->RegCr3;
            }
        }

        //  Get all floating point registers (FPU)
        if (registers.find("fctrl") != registers.end())
        {
            pContext->ControlWord = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["fctrl"]));
            pContext->StatusWord = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["fstat"]));
            pContext->TagWord = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["ftag"]));
            pContext->ErrorOffset = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["fioff"]));
            pContext->ErrorSelector = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["fiseg"]));
            pContext->DataOffset = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["fooff"]));
            pContext->DataSelector = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["foseg"]));
        }

        //  Are the GDT & IDT system register present?
        if (registers.find("gdtrbase") != registers.end())
        {
            pContext->GDTBase = GdbSrvController::ParseRegisterValue(registers["gdtrbase"]);
            pContext->GDTLimit = GdbSrvController::ParseRegisterValue32(registers["gdtrlimit"]);
        }

        if (registers.find("idtrbase") != registers.end())
        {
            pContext->IDTBase = GdbSrvController::ParseRegisterValue(registers["idtrbase"]);
            pContext->IDTLimit = GdbSrvController::ParseRegisterValue32(registers["idtrlimit"]);
        }

        //  x87 registers (FPU)
        for (int index = 0; index < s_numberFPRegList; ++index)
        {
            std::string regName(s_fpRegList[index]);
            GdbSrvController::ParseRegisterVariableSize(registers[regName],
                reinterpret_cast<BYTE*>(&pContext->RegisterArea[index * s_numberOfBytesCoprocessorRegister]),
                s_numberOfBytesCoprocessorRegister);
        }
        pContext->RegGroupSelection.fFloatingPointRegs = TRUE;

        //  Get X64 SSE registers if the x64 SSE context enabled?
        if (m_fEnableSSEContext)
        {
            registers = pController->QueryRegisters(processorNumber, s_sseX64RegList, s_numberOfSseX64Registers);
            const int numberOfBytesSseX64Registers = sizeof(pContext->RegSSE[0]);
            for (int index = 0; index < s_numberOfSseX64Registers; ++index)
            {
                std::string regName(s_sseX64RegList[index]);
                GdbSrvController::ParseRegisterVariableSize(registers[regName],
                    reinterpret_cast<BYTE*>(&pContext->RegSSE[index]),
                    numberOfBytesSseX64Registers);
            }
            pContext->RegMXCSR = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["mxcsr"]));
            pContext->RegGroupSelection.fSSERegisters = TRUE;
        }

        pContext->RegGroupSelection.fSegmentDescriptors = FALSE;
        PopulateAmd64DebugRegisters(processorNumber, pContext);
        pContext->RegGroupSelection.fDebugRegs = TRUE;

        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::SetContextEx(_In_ DWORD processorNumber, _In_ const CONTEXT_X86_64 *pContext)
{
    if (pContext == nullptr)
    {
        return E_POINTER;
    }

    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }
        pController->StopTargetAtRun();
        std::map<std::string, ULONGLONG> registers;
        if (pContext->RegGroupSelection.fIntegerRegs)
        {
            registers["rax"] = pContext->Rax;
            registers["rbx"] = pContext->Rbx;
            registers["rcx"] = pContext->Rcx;
            registers["rdx"] = pContext->Rdx;
            registers["rsi"] = pContext->Rsi;
            registers["rdi"] = pContext->Rdi;
            registers["rip"] = pContext->Rip;
            m_lastPcAddress  = pContext->Rip;
            registers["rsp"] = pContext->Rsp;
            registers["rbp"] = pContext->Rbp;
            registers["r8"] = pContext->R8;
            registers["r9"] = pContext->R9;
            registers["r10"] = pContext->R10;
            registers["r11"] = pContext->R11;
            registers["r12"] = pContext->R12;
            registers["r13"] = pContext->R13;
            registers["r14"] = pContext->R14;
            registers["r15"] = pContext->R15;
            registers["eflags"] = pContext->EFlags;
        }

        if (pContext->RegGroupSelection.fSegmentRegs)
        {
            registers["cs"] = pContext->SegCs;
            registers["ss"] = pContext->SegSs;
            registers["ds"] = pContext->SegDs;
            registers["es"] = pContext->SegEs;
            registers["fs"] = pContext->SegFs;
            registers["gs"] = pContext->SegGs;
        }

        if (pContext->RegGroupSelection.fFloatingPointRegs)
        {
            registers["fctrl"] = pContext->ControlWord;
            registers["fstat"] = pContext->StatusWord;
            registers["ftag"] = pContext->TagWord;
            registers["fioff"] = pContext->ErrorOffset;
            registers["fiseg"] = pContext->ErrorSelector;
            registers["fooff"] = pContext->DataOffset;
            registers["foseg"] = pContext->DataSelector;
        }

        //  Control registers
        if (pContext->RegGroupSelection.fSystemRegisters)
        {
            // VMware monitor values are readable but have no verified write path.
            // Accept DbgEng's unchanged context round trip and reject real writes.
            if (processorNumber >= m_amd64SystemRegisters.size())
            {
                return E_NOTIMPL;
            }

            const Amd64SystemRegisters& current = m_amd64SystemRegisters[processorNumber];
            if (!current.valid ||
                current.cr0 != pContext->RegCr0 ||
                current.cr2 != pContext->RegCr2 ||
                current.cr3 != pContext->RegCr3 ||
                current.cr4 != pContext->RegCr4 ||
                current.cr8 != pContext->RegCr8)
            {
                return E_NOTIMPL;
            }
        }
        if (!registers.empty())
        {
            pController->SetRegisters(processorNumber, registers, false);
        }
        registers.clear();

        //  Floating point registers
        if (pContext->RegGroupSelection.fFloatingPointRegs)
        {
            for (int index = 0; index < s_numberFPRegList; ++index)
            {
                std::string regName(s_fpRegList[index]);
                registers[regName] = reinterpret_cast<ULONGLONG>(&pContext->RegisterArea[index * s_numberOfBytesCoprocessorRegister]);
            }
            pController->SetRegisters(processorNumber, registers, true);
            registers.clear();
        }

        //  SSE x64 registers
        if (m_fEnableSSEContext)
        {
            for (int index = 0; index < s_numberOfSseX64Registers; ++index)
            {
                std::string regName(s_sseX64RegList[index]);
                registers[regName] = reinterpret_cast<ULONGLONG>(&pContext->RegSSE[index]);
            }
            pController->SetRegisters(processorNumber, registers, true);
            registers.clear();
        }

        if (pContext->RegGroupSelection.fDebugRegs)
        {
            SynchronizeAmd64DebugRegisters(*pContext, pController);
        }

        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}

// ------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::GetContextEx(_In_ DWORD processorNumber, _Inout_ PCONTEXT_X86_EX pContext)
{
    if (pContext == nullptr)
    {
        return E_POINTER;
    }

    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }
        pController->StopTargetAtRun();
        memset(pContext, 0, sizeof(CONTEXT_X86_EX));

        pContext->DescriptorCs.Flags = static_cast<DWORD>(X86_DESC_FLAGS);
        pContext->DescriptorSs.Flags = static_cast<DWORD>(X86_DESC_FLAGS);
        pContext->DescriptorGs.Flags = static_cast<DWORD>(X86_DESC_FLAGS);
        pContext->DescriptorFs.Flags = static_cast<DWORD>(X86_DESC_FLAGS);
        pContext->DescriptorEs.Flags = static_cast<DWORD>(X86_DESC_FLAGS);
        pContext->DescriptorDs.Flags = static_cast<DWORD>(X86_DESC_FLAGS);

        std::map<std::string, std::string> registers = pController->QueryAllRegisters(processorNumber);
        //  Get core integer registers
        GetX86CoreRegisters(registers, pContext);
        //  Get the 80387 Copreocessor registers
        GetFPCoprocessorRegisters(registers, processorNumber, pController, reinterpret_cast<PVOID>(pContext));
        //  Is the SSE context enabled?
        if (m_fEnableSSEContext)
        {
            //  Get the SSE registers
            GetSSERegisters(processorNumber, pController, pContext);
        }

        pContext->RegGroupSelection.fDebugRegs = FALSE;
        pContext->RegGroupSelection.fSystemRegisters = FALSE;
        pContext->RegGroupSelection.fSegmentDescriptors = FALSE;

        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::SetContextEx(_In_ DWORD processorNumber, _In_ const CONTEXT_X86_EX *pContext)
{
    if (pContext == nullptr)
    {
        return E_POINTER;
    }

    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }
        pController->StopTargetAtRun();
        SetX86CoreRegisters(processorNumber, pContext, pController);

        SetFPCoprocessorRegisters(processorNumber, reinterpret_cast<const VOID *>(pContext), pController);

        //  Is the SSE context enabled?
        if (m_fEnableSSEContext)
        {
            SetSSERegisters(processorNumber, reinterpret_cast<const VOID *>(pContext), pController);
        }

        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::GetContextEx(_In_ DWORD processorNumber, _Inout_ PCONTEXT_ARMV8ARCH64 pContext)
{
    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }
        pController->StopTargetAtRun();
        memset(pContext, 0, sizeof(CONTEXT_ARMV8ARCH64));

        std::map<std::string, std::string> registers = pController->QueryAllRegisters(processorNumber);

        for (int i = 0; i < ARMV8ARCH64_MAX_INTERGER_REGISTERS; ++i)
        {
            char registerNameStr[4] = {0};
            sprintf_s(registerNameStr, _countof(registerNameStr), "X%d", i);
            std::string registerName(registerNameStr);
            pContext->X[i] = GdbSrvController::ParseRegisterValue(registers[registerName]);
        }
        pContext->Fp = GdbSrvController::ParseRegisterValue(registers["fp"]);
        pContext->Lr = GdbSrvController::ParseRegisterValue(registers["lr"]);
        pContext->Sp = GdbSrvController::ParseRegisterValue(registers["sp"]);
        pContext->Pc = GdbSrvController::ParseRegisterValue(registers["pc"]);
        pContext->Psr = GdbSrvController::ParseRegisterValue(registers["cpsr"]);
        m_lastPcAddress = pContext->Pc;
        m_lastPSRvalue = pContext->Psr;

        pContext->RegGroupSelection.fControlRegs = TRUE;
        pContext->RegGroupSelection.fIntegerRegs = TRUE;
        pContext->RegGroupSelection.fFloatingPointRegs = FALSE;
        pContext->RegGroupSelection.fDebugRegs = FALSE;

        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::SetContextEx(_In_ DWORD processorNumber, _In_ const CONTEXT_ARMV8ARCH64 *pContext)
{
    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }
        pController->StopTargetAtRun();

        std::map<std::string, ULONGLONG> registers;
        if (pContext->RegGroupSelection.fIntegerRegs)
        {
            for (int i = 0; i < ARMV8ARCH64_MAX_INTERGER_REGISTERS; ++i)
            {
                char registerNameStr[4] = {0};
                sprintf_s(registerNameStr, _countof(registerNameStr), "X%d", i);
                std::string registerName(registerNameStr);
                registers[registerName] = pContext->X[i];
            }
            registers["fp"] = pContext->Fp;
            registers["lr"] = pContext->Lr;
        }

        if (pContext->RegGroupSelection.fControlRegs)
        {
            registers["pc"] = pContext->Pc;
            registers["sp"] = pContext->Sp;
            registers["cpsr"] = pContext->Psr;
            m_lastPcAddress  = pContext->Pc;
        }
        pController->SetRegisters(processorNumber, registers, false);

        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}

ADDRESS_TYPE CLiveExdiGdbSrvServer::GetCurrentExecutionAddress(_Out_ DWORD *pProcessorNumberOfLastEvent)
{
    assert(pProcessorNumberOfLastEvent != nullptr);
    AsynchronousGdbSrvController * pController = GetGdbSrvController();
    assert(pController != nullptr);

    *pProcessorNumberOfLastEvent = pController->GetLastKnownActiveCpu();
    ADDRESS_TYPE result;
    std::map<std::string, std::string> registers = pController->QueryAllRegisters(*pProcessorNumberOfLastEvent);

    if (m_detectedProcessorFamily == PROCESSOR_FAMILY_ARM || m_detectedProcessorFamily == PROCESSOR_FAMILY_ARMV8ARCH64)
    {
        result = GdbSrvController::ParseRegisterValue(registers["pc"]);
    }
    else if (m_detectedProcessorFamily == PROCESSOR_FAMILY_X86)
    {
        if (m_targetProcessorArch == X86_ARCH)
        {
            result = GdbSrvController::ParseRegisterValue(registers["Eip"]);
        }
        else
        {
            result = GdbSrvController::ParseRegisterValue(registers["rip"]);
        }
    }
    else
    {
        throw std::exception("Unknown CPU architecture. Please add support for it");
    }
    m_lastPcAddress = result;
    return result;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::SetKeepaliveInterface(/* [in] */ IeXdiKeepaliveInterface3 *pKeepalive)
{
    m_pKeepaliveInterface = pKeepalive;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::OnAsynchronousCommandCompleted()
{
    if (m_pRunNotificationListener != nullptr)
    {
        HALT_REASON_TYPE haltReason = hrUnknown;

        DWORD eventProcessor = 0;
        ADDRESS_TYPE currentAddress = ParseAsynchronousCommandResult(&eventProcessor, &haltReason);
        if (m_lastResumingCommandWasStep)
        {
            haltReason = hrStep;
        }

        m_targetIsRunning = false;
        if (currentAddress != 0)
        {
            m_pRunNotificationListener->NotifyRunStateChange(rsHalted, haltReason, currentAddress, 0, eventProcessor);
            return S_OK;
        }
    }
    return E_FAIL;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::PerformKeepaliveChecks(void)
{
    if (m_pKeepaliveInterface == nullptr)
    {
        return S_FALSE;
    }

    AsynchronousGdbSrvController * pController = GetGdbSrvController();
    if (pController == nullptr)
    {
        return E_POINTER;
    }

    // Get the GdbServer connection status
    bool isGdbServerDown = false;
    HRESULT gdbServerError = S_OK;;
    if (pController->CheckGdbSrvAlive(gdbServerError))
    {
        if (gdbServerError == ERROR_OPERATION_ABORTED)
        {
            //  Close the connection with the GdbServer
            pController->ShutdownGdbSrv();
            isGdbServerDown = true;
        }
    }

    HRESULT result = m_pKeepaliveInterface->IsDebugSessionAlive();
    if (FAILED(result) || isGdbServerDown)
    {
        TCHAR fileName[MAX_PATH];
        bool lostConnection = ((gdbServerError == ERROR_OPERATION_ABORTED) || HRESULT_FACILITY(result) == FACILITY_WIN32 &&
                              ((HRESULT_CODE(result) == RPC_S_CALL_FAILED) || (HRESULT_CODE(result) == RPC_S_SERVER_UNAVAILABLE)));

        if (lostConnection && GetModuleFileName(GetModuleHandle(0), fileName, _countof(fileName)))
        {
            TCHAR *pLastSlash = _tcsrchr(fileName, '\\');
            if (pLastSlash && !_tcsicmp(pLastSlash + 1, _T("dllhost.exe")))
            {
                ExitProcess(result);
            }
        }
    }
    return S_OK;
}

HRESULT CLiveExdiGdbSrvServer::SetGdbServerParameters()
{
    try
    {
        TCHAR configXmlFile[MAX_PATH + 1];
        DWORD fileNameLength = GetEnvironmentVariable(_T("EXDI_GDBSRV_XML_CONFIG_FILE"),
                                                      configXmlFile, _countof(configXmlFile));
        if (fileNameLength == 0)
        {
            MessageBox(0, _T("Error: the EXDI_GDBSRV_XML_CONFIG_FILE environment variable is not defined.\n")
                          _T("The Exdi-GdbServer won't continue at this point.\n")
                          _T("Please set the full path to the Exdi xml configuration file."), _T("EXDI-GdbServer"), MB_ICONERROR);
            return E_ABORT;
        }

        ConfigExdiGdbServerHelper & cfgData = ConfigExdiGdbServerHelper::GetInstanceCfgExdiGdbServer(configXmlFile);
        m_targetProcessorArch = cfgData.GetTargetArchitecture();
        m_detectedProcessorFamily = cfgData.GetTargetFamily();
        m_fDisplayCommData = cfgData.GetDisplayCommPacketsCharacters();
        m_fEnableSSEContext = cfgData.GetIntelSseContext();
        m_heuristicChunkSize = cfgData.GetHeuristicScanMemorySize();
        m_RequireMemoryAccessByPA = cfgData.GetServerRequirePAMemoryAccess();
        unsigned numberOfCores = cfgData.GetNumberOfCores();
        std::vector<std::wstring> coreConnections;
        cfgData.GetGdbServerConnectionParameters(coreConnections);
        if (coreConnections.size() != numberOfCores)
        {
            MessageBox(0, _T("Error: the number of cores does not match with the number of connection strings in the configuration xml file."),
                       _T("EXDI-GdbServer"), MB_ICONERROR);
            return E_ABORT;
        }

        m_pGdbSrvController = AsynchronousGdbSrvController::Create(coreConnections);
        m_pGdbSrvController->SetTargetArchitecture(m_targetProcessorArch);
        m_pGdbSrvController->SetTargetProcessorFamilyByTargetArch(m_targetProcessorArch);
        if (m_fDisplayCommData)
        {
            m_pGdbSrvController->SetTextHandler(new CommandLogger(true));
        }

        WCHAR systemRegMapXmlFile[MAX_PATH + 1];
        fileNameLength = GetEnvironmentVariable(_T("EXDI_SYSTEM_REGISTERS_MAP_XML_FILE"),
            systemRegMapXmlFile, _countof(systemRegMapXmlFile));
        if (fileNameLength != 0)
        {
            m_pGdbSrvController->SetSystemRegisterXmlFile(systemRegMapXmlFile);
        }
        else
        {
            MessageBox(0, _T("Error: the EXDI_SYSTEM_REGISTERS_MAP_XML_FILE environment variable is not defined.\n")
                _T("rdmsr/wrmsr functions won't work at this point.\n")
                _T("Please set the full path to the SYSTEMREGISTERS.XML file."), _T("EXDI-GdbServer"), MB_ICONERROR);
        }

        // Check server asynchronous command time interval mode
        if (m_pGdbSrvController->IsServerSlowAsyncResponseMode())
        {
            m_pGdbSrvController->SetSleepAsyncCmdInterval(c_asyncSlowSrvResponsePauseMs);
        }

        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}

VOID CALLBACK CLiveExdiGdbSrvServer::TimerCallback(_In_  HWND hwnd, _In_  UINT uMsg, _In_  UINT_PTR idEvent, _In_  DWORD dwTime)
{
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(uMsg);
    UNREFERENCED_PARAMETER(dwTime);

    const auto owner = g_timerOwners.find(idEvent);
    if (owner == g_timerOwners.end())
    {
        return;
    }

    CLiveExdiGdbSrvServer* server = owner->second;
    AsynchronousGdbSrvController* controller = server->GetGdbSrvController();
    if (controller == nullptr)
    {
        return;
    }

    const HRESULT keepaliveResult = server->PerformKeepaliveChecks();
    if (FAILED(keepaliveResult))
    {
        AppendVMwareLog("keepalive callback failed hr=%08x\n", keepaliveResult);
    }

    if (!server->m_targetIsRunning || controller->IsAsynchronousCommandInProgress())
    {
        return;
    }

    try
    {
        if (controller->GetAsynchronousCommandResult(0, nullptr))
        {
            const HRESULT notificationResult = server->OnAsynchronousCommandCompleted();
            if (FAILED(notificationResult))
            {
                AppendVMwareLog("asynchronous completion callback failed hr=%08x\n", notificationResult);
            }
        }
    }
    catch (const std::exception& error)
    {
        AppendVMwareLog("asynchronous completion polling failed: %s\n", error.what());
    }
    catch (...)
    {
        AppendVMwareLog("asynchronous completion polling failed with an unknown exception\n");
    }
}

HRESULT CLiveExdiGdbSrvServer::SetGdbServerConnection(void)
{
    try
    {
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }

        //  Configure the GdbServer communication session.
        if (!pController->ConfigureGdbSrvCommSession(m_fDisplayCommData, C_ALLCORES))
        {
            //  Failed configuring the session.
            MessageBox(0, _T("Error: Unable to configure the GdbServer session.")
                          _T("Please verify the SendPacketTimeout & ReceivePacketTimeout timeout value (msec unit)")
                          _T("attributes for the current selected GDB server at the exdiconfigdata.xml file."),
                          _T("EXDI-GdbServer"), MB_ICONERROR);
            return E_ABORT;
        }

        //  Execute the connection to the GdbServer
        if (!pController->ConnectGdbSrv())
        {
            //  Failed connecting to the GdbServer.
            MessageBox(0, _T("Error: Unable to establish a connection with the GdbServer.")
                          _T("Please verify the connection string <hostname/ip>:portnumber."),
                          _T("EXDI-GdbServer"), MB_ICONERROR);
            return E_ABORT;
        }

        HRESULT result = E_FAIL;

        //  Establish the handshaking with the GdbServer.
        //  Request the set of features supported by the GdbServer
        if (pController->ReqGdbServerSupportedFeatures())
        {
            //  Ensure that the target architecture matches with the current GDB server
            m_targetProcessorArch = m_pGdbSrvController->GetTargetArchitecture();
            m_detectedProcessorFamily = m_pGdbSrvController->GetProcessorFamilyArchitecture();
            //  Is the target broken because the GdbServer sends a break request?
            if (pController->IsTargetHalted())
            {
                result = S_OK;
            }
        }
        return result;
    }
    CATCH_AND_RETURN_HRESULT;
}

ADDRESS_TYPE CLiveExdiGdbSrvServer::ParseAsynchronousCommandResult(_Out_ DWORD * pProcessorNumberOfLastEvent, _Out_ HALT_REASON_TYPE * pHaltReason)
{
    assert(pProcessorNumberOfLastEvent != nullptr);
    AsynchronousGdbSrvController * pController = GetGdbSrvController();
    assert(pController != nullptr);

    ADDRESS_TYPE currentPcAddress = 0;
    if (pController->GetAsynchronousCmdStopReplyPacket())
    {
        int attempts = 0;
        bool isWaitingOnStopReply = false;
        ULONG totalPackets = 0;
        do
        {
            StopReplyPacketStruct stopReply = {0};
            const std::string reply = pController->GetCommandResult();
            bool isParsed = pController->HandleAsynchronousCommandResponse(reply, &stopReply);
            if (isParsed)
            {
                attempts = 0;
                const bool hasValidEventProcessor =
                    stopReply.status.isThreadFound &&
                    stopReply.processorNumber < pController->GetProcessorCount();
                const bool isDataBreakpointHit =
                    stopReply.status.isTAAPacket &&
                    stopReply.status.isWatchpointFound &&
                    hasValidEventProcessor &&
                    RecordAmd64DataBreakpointHit(
                        stopReply.watchpointAddress,
                        stopReply.processorNumber);
                if (stopReply.status.isTAAPacket &&
                    stopReply.status.isWatchpointFound &&
                    !hasValidEventProcessor)
                {
                    AppendVMwareLog(
                        "virtual-dr watchpoint hit without valid processor reported=%I64x processor=%u\n",
                        static_cast<ULONGLONG>(stopReply.watchpointAddress),
                        stopReply.processorNumber);
                }
                //  Is it a OXX console packet?
                if (stopReply.status.isOXXPacket)
                {
                    //  Try to display the GDB server ouput message if there is an attached text console.
                    pController->DisplayConsoleMessage(reply);
                    //  Post another receive request on the packet buffer
                    pController->ContinueWaitingOnStopReplyPacket();
                    isWaitingOnStopReply = true;
                }
                //  Is it a T packet?
                else if (stopReply.status.isTAAPacket)
                {
                    if (stopReply.status.isPcRegFound)
                    {
                        assert(stopReply.currentAddress != 0);
                        currentPcAddress = m_lastPcAddress = stopReply.currentAddress;
                    }
                    else
                    {
                        //  The packet didn't contain the PC, but we'd better find out what it is, so we can inform the debugger
                        DWORD pcAddressRequest;
                        currentPcAddress = m_lastPcAddress = GetCurrentExecutionAddress(&pcAddressRequest);
                    }

                    if (hasValidEventProcessor)
                    {
                        *pProcessorNumberOfLastEvent = stopReply.processorNumber;
                    }
                    else
                    {
                        *pProcessorNumberOfLastEvent = pController->GetLastKnownActiveCpu();
                    }
                    isWaitingOnStopReply = false;
                }
                //  Is it a S AA packet?
                else if (stopReply.status.isSAAPacket)
                {
                    //  There is a no any processor number or pc adddress in the response
                    if (stopReply.status.isPowerDown)
                    {
                        MessageBox(0, _T("The Target is running or it is in a power down state."),nullptr, MB_ICONERROR);
                    }
                    stopReply.currentAddress = m_lastPcAddress;
                    *pProcessorNumberOfLastEvent = pController->GetLastKnownActiveCpu();
                    isWaitingOnStopReply = false;
                }
                // Is it an "OK" response w/o any other field (e.g. OpenOCD can send "OK" after 's'/'g')?
                else if (stopReply.status.isCoreRunning)
                {
                    //  Post another receive request on the packet buffer, since there is still no
                    //  trace of the current thread-core/Pc address packet.
                    pController->ContinueWaitingOnStopReplyPacket();
                    isWaitingOnStopReply = true;
                }

                if (!isWaitingOnStopReply)
                {
                    //  Convert the stop reason code
                    switch (stopReply.stopReason)
                    {
                    case TARGET_BREAK_SIGINT:
                       *pHaltReason = hrUser;
                       break;
                    case TARGET_BREAK_SIGTRAP:
                       // DbgEng consults virtual DR6/DR7 for a step-like data-break event.
                       *pHaltReason = isDataBreakpointHit ? hrStep : hrBp;
                       break;
                    default:
                       *pHaltReason = hrUnknown;
                    }
                    pController->ResetAsynchronousCmdStopReplyPacket();
                }
            }
            else
            {
                Sleep(pController->GetSleepAsyncCmdInterval());
            }
        }
        while (isWaitingOnStopReply &&
            (attempts++ < c_attemptsWaitingOnPendingResponse) &&
            (totalPackets < c_maximumReplyPacketsInResponse));
    }
    else
    {
        //  This can happen only if there was a previously handled Halt event.
        currentPcAddress = m_lastPcAddress;
    }
    return currentPcAddress;
}

void CLiveExdiGdbSrvServer::GetX86CoreRegisters(_In_ std::map<std::string, std::string> &registers,
                                                      _Out_ CONTEXT_X86_EX * pContext)
{
    assert(pContext != nullptr);

    pContext->Eax = GdbSrvController::ParseRegisterValue32(registers["Eax"]);
    pContext->Ebx = GdbSrvController::ParseRegisterValue32(registers["Ebx"]);
    pContext->Ecx = GdbSrvController::ParseRegisterValue32(registers["Ecx"]);
    pContext->Edx = GdbSrvController::ParseRegisterValue32(registers["Edx"]);
    pContext->Esi = GdbSrvController::ParseRegisterValue32(registers["Esi"]);
    pContext->Edi = GdbSrvController::ParseRegisterValue32(registers["Edi"]);
    pContext->Eip = GdbSrvController::ParseRegisterValue32(registers["Eip"]);
    m_lastPcAddress = pContext->Eip;
    pContext->Esp = GdbSrvController::ParseRegisterValue32(registers["Esp"]);
    pContext->Ebp = GdbSrvController::ParseRegisterValue32(registers["Ebp"]);
    pContext->EFlags = GdbSrvController::ParseRegisterValue32(registers["EFlags"]);
    pContext->RegGroupSelection.fIntegerRegs = TRUE;

    pContext->SegCs = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["SegCs"]));
    pContext->SegSs = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["SegSs"]));
    pContext->RegGroupSelection.fControlRegs = TRUE;

    pContext->SegDs = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["SegDs"]));
    pContext->SegEs = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["SegEs"]));
    pContext->SegFs = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["SegFs"]));
    pContext->SegGs = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["SegGs"]));
    pContext->RegGroupSelection.fSegmentRegs = TRUE;
}

void CLiveExdiGdbSrvServer::GetFPCoprocessorRegisters(_In_ std::map<std::string, std::string> &registers,
                                                            _In_ DWORD processorNumber,
                                                            _In_ AsynchronousGdbSrvController * const pController,
                                                            _Out_ PVOID pContext)
{
    assert(pContext != nullptr);

    PCONTEXT_X86_EX pContextFP = reinterpret_cast<PCONTEXT_X86_EX>(pContext);

    pContextFP->ControlWord = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["ControlWord"]));
    pContextFP->StatusWord = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["StatusWord"]));
    pContextFP->TagWord = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["TagWord"]));
    pContextFP->ErrorOffset = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["ErrorOffset"]));
    pContextFP->ErrorSelector = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["ErrorSelector"]));
    pContextFP->DataOffset = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["DataOffset"]));
    pContextFP->DataSelector = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(registers["DataSelector"]));

    for (int index = 0; index < s_numberFPRegList; ++index)
    {
        std::string regName(s_fpRegList[index]);
        GdbSrvController::ParseRegisterVariableSize(registers[regName],
                                                    reinterpret_cast<BYTE *>(&pContextFP->RegisterArea[index * s_numberOfBytesCoprocessorRegister]),
                                                    s_numberOfBytesCoprocessorRegister);
    }

    const char * fpNpxStateRegister[] = {"Cr0NpxState"};
    std::map<std::string, std::string> fpNpxStateRegValue = pController->QueryRegisters(processorNumber, fpNpxStateRegister, ARRAYSIZE(fpNpxStateRegister));
    pContextFP->Cr0NpxState = static_cast<DWORD>(GdbSrvController::ParseRegisterValue32(fpNpxStateRegValue[fpNpxStateRegister[0]]));

    pContextFP->RegGroupSelection.fFloatingPointRegs = TRUE;
}

void CLiveExdiGdbSrvServer::GetSSERegisters(_In_ DWORD processorNumber,
                                                  _In_ AsynchronousGdbSrvController * const pController,
                                                  _Out_ PVOID pContext)
{
    assert(pController != nullptr && pContext != nullptr);

    std::map<std::string, std::string> registers = pController->QueryRegisters(processorNumber, s_sseRegList, s_numberOfSseRegisters);

    PCONTEXT_X86_EX pContextSSE = reinterpret_cast<PCONTEXT_X86_EX>(pContext);
    const int numberOfBytesSseRegisters = sizeof(pContextSSE->Sse[0]);

    for (int index = 0; index < s_numberOfSseRegisters; ++index)
    {
        std::string regName(s_sseRegList[index]);
        GdbSrvController::ParseRegisterVariableSize(registers[regName],
                                                    reinterpret_cast<BYTE *>(&pContextSSE->Sse[index]),
                                                    numberOfBytesSseRegisters);
    }
    pContextSSE->RegGroupSelection.fSSERegisters = TRUE;
}

void CLiveExdiGdbSrvServer::SetX86CoreRegisters(_In_ DWORD processorNumber, _In_ const CONTEXT_X86_EX * pContext,
                                                      _In_ AsynchronousGdbSrvController * const pController)
{
    assert(pContext != nullptr && pController != nullptr);

    std::map<std::string, ULONGLONG> registers;

    if (pContext->RegGroupSelection.fIntegerRegs)
    {
        registers["Eax"] = pContext->Eax;
        registers["Ebx"] = pContext->Ebx;
        registers["Ecx"] = pContext->Ecx;
        registers["Edx"] = pContext->Edx;
        registers["Esi"] = pContext->Esi;
        registers["Edi"] = pContext->Edi;
        registers["Eip"] = pContext->Eip;
        m_lastPcAddress  = pContext->Eip;
        registers["Esp"] = pContext->Esp;
        registers["Ebp"] = pContext->Ebp;
    }

    if (pContext->RegGroupSelection.fSegmentRegs)
    {
        registers["SegCs"] = pContext->SegCs;
        registers["SegSs"] = pContext->SegSs;
        registers["SegDs"] = pContext->SegDs;
        registers["SegEs"] = pContext->SegEs;
        registers["SegFs"] = pContext->SegFs;
        registers["SegGs"] = pContext->SegGs;
    }

    if (pContext->RegGroupSelection.fFloatingPointRegs)
    {
        registers["ControlWord"] = pContext->ControlWord;
        registers["StatusWord"] = pContext->StatusWord;
        registers["TagWord"] = pContext->TagWord;
        registers["ErrorOffset"] = pContext->ErrorOffset;
        registers["ErrorSelector"] = pContext->ErrorSelector;
        registers["DataOffset"] = pContext->DataOffset;
        registers["DataSelector"] = pContext->DataSelector;
        registers["Cr0NpxState"] = pContext->Cr0NpxState;
    }

    pController->SetRegisters(processorNumber, registers, false);
}

void CLiveExdiGdbSrvServer::SetFPCoprocessorRegisters(_In_ DWORD processorNumber, _In_ const VOID * pContext,
                                                            _In_ AsynchronousGdbSrvController * const pController)
{
    assert(pContext != nullptr && pController != nullptr);

    const CONTEXT_X86_EX * pContextFP = reinterpret_cast<const CONTEXT_X86_EX *>(pContext);
    if (pContextFP->RegGroupSelection.fFloatingPointRegs)
    {
        std::map<std::string, ULONGLONG> registers;

        for (int index = 0; index < s_numberFPRegList; ++index)
        {
            std::string regName(s_fpRegList[index]);
            registers[regName] = reinterpret_cast<ULONGLONG>(&pContextFP->RegisterArea[index * s_numberOfBytesCoprocessorRegister]);
        }
        pController->SetRegisters(processorNumber, registers, true);
    }
}

void CLiveExdiGdbSrvServer::SetSSERegisters(_In_ DWORD processorNumber, _In_ const VOID * pContext,
                                                  _In_ AsynchronousGdbSrvController * const pController)
{
    assert(pContext != nullptr && pController != nullptr);

    const CONTEXT_X86_EX * pContextSse = reinterpret_cast<const CONTEXT_X86_EX *>(pContext);
    if (pContextSse->RegGroupSelection.fSSERegisters)
    {
        std::map<std::string, ULONGLONG> registers;

        for (int index = 0; index < s_numberOfSseRegisters; ++index)
        {
            std::string regName(s_sseRegList[index]);
            registers[regName] = reinterpret_cast<ULONGLONG>(&pContextSse->Sse[index]);
        }
        pController->SetRegisters(processorNumber, registers, true);
    }
}

void CLiveExdiGdbSrvServer::GetNeonRegisters(_In_ AsynchronousGdbSrvController * const pController,
                                                   _In_ std::map<std::string, std::string> &registers,
                                                   _Out_ PVOID pContext)
{
    assert(pController != nullptr && pContext != nullptr);

    std::unique_ptr<char> neonNameRegArray[EXDI_ARM_MAX_NEON_FP_REGISTERS];
    std::string firstNeonRegister("d0");
    pController->CreateNeonRegisterNameArray(firstNeonRegister, neonNameRegArray, EXDI_ARM_MAX_NEON_FP_REGISTERS);

    PCONTEXT_ARM4 pContextArm = reinterpret_cast<PCONTEXT_ARM4>(pContext);
    const int numberOfBytesNeonRegisters = sizeof(pContextArm->D[0]);
    for (size_t index = 0; index < EXDI_ARM_MAX_NEON_FP_REGISTERS; ++index)
    {
        std::string regName(neonNameRegArray[index].get());
        GdbSrvController::ParseRegisterVariableSize(registers[regName],
                                                    reinterpret_cast<BYTE *>(&pContextArm->D[index]),
                                                    numberOfBytesNeonRegisters);
    }
    pContextArm->RegGroupSelection.fFloatingPointRegs = TRUE;
}

void CLiveExdiGdbSrvServer::SetNeonRegisters(_In_ DWORD processorNumber, _In_ const VOID * pContext,
                                             _In_ AsynchronousGdbSrvController * const pController)
{
    assert(pContext != nullptr && pController != nullptr);

    std::unique_ptr<char> neonNameRegArray[EXDI_ARM_MAX_NEON_FP_REGISTERS];
    std::string firstNeonRegister("d0");
    pController->CreateNeonRegisterNameArray(firstNeonRegister, neonNameRegArray, EXDI_ARM_MAX_NEON_FP_REGISTERS);

    const CONTEXT_ARM4 * pContextArm = reinterpret_cast<const CONTEXT_ARM4 *>(pContext);
    if (pContextArm->RegGroupSelection.fFloatingPointRegs)
    {
        std::map<std::string, ULONGLONG> registers;
        for (int index = 0; index < EXDI_ARM_MAX_NEON_FP_REGISTERS; ++index)
        {
            std::string regName(neonNameRegArray[index].get());
            registers[regName] = reinterpret_cast<ULONGLONG>(&pContextArm->D[index]);
        }
        pController->SetRegisters(processorNumber, registers, true);
    }
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::ExecuteExdiComponentFunction(
    /* [in] */ ExdiComponentFunctionType type,
    /* [in] */ DWORD dwProcessorNumber,
    /* [in] */ LPCWSTR pFunctionToExecute)
{
    try
    {
        if (pFunctionToExecute == nullptr)
        {
            return E_POINTER;
        }
        if (type != exdiComponentSession)
        {
            return E_INVALIDARG;
        }
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }
        if (_wcsicmp(pFunctionToExecute, L"close") == 0)
        {
            if (m_targetIsRunning)
            {
                return HRESULT_FROM_WIN32(ERROR_BUSY);
            }
            RemoveAllAmd64DataBreakpoints();
        }
        if (!pController->ExecuteExdiFunction(dwProcessorNumber, pFunctionToExecute))
        {
            return E_FAIL;
        }
        return S_OK;
    }
    CATCH_AND_RETURN_HRESULT;
}

HRESULT STDMETHODCALLTYPE CLiveExdiGdbSrvServer::ExecuteTargetEntityFunction(
    /* [in] */ ExdiComponentFunctionType type,
    /* [in] */ DWORD dwProcessorNumber,
    /* [in] */ LPCWSTR pFunctionToExecute,
    /* [out] */ SAFEARRAY ** pFunctionResponseBuffer)
{
    try
    {
        if (pFunctionToExecute == nullptr)
        {
            return E_POINTER;
        }
        if (type != exdiTargetEntity || dwProcessorNumber == C_ALLCORES)
        {
            return E_INVALIDARG;
        }
        AsynchronousGdbSrvController * pController = GetGdbSrvController();
        if (pController == nullptr)
        {
            return E_POINTER;
        }
        SimpleCharBuffer buffer = pController->ExecuteExdiGdbSrvMonitor(dwProcessorNumber, pFunctionToExecute);
        return SafeArrayFromByteArray(buffer.GetInternalBuffer(), buffer.GetLength(), pFunctionResponseBuffer);
    }
    CATCH_AND_RETURN_HRESULT;
}

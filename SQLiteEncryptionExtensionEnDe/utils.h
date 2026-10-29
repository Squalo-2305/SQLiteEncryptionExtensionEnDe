#pragma once

#include <string>

enum class OperationMode
{
    Encrypt,
    Decrypt
};

enum class InputType
{
    File,
    Folder
};

struct CliOptions
{
    OperationMode mode = OperationMode::Encrypt;
    InputType inputType = InputType::File;
    std::wstring path;
    std::string hexKey;
};

bool parseCommandLine(
    int argc,
    wchar_t* argv[],
    CliOptions& options,
    std::wstring& error);

std::string toUtf8(const std::wstring& value);
void secureClear(std::string& value);

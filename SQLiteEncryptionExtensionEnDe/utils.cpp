#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "utils.h"

#include <algorithm>
#include <cwchar>
#include <sstream>
#include <utility>

namespace
{
    constexpr wchar_t Usage[] =
        L"Usage: SQLiteEncryptionExtensionEnDe.exe <en|de> -t <file|folder> -p <path> -k <key>\n"
        L"Options -t, -p, and -k can be provided in any order.\n"
        L"Use hex:<value> for a hexadecimal key; keys without that prefix are UTF-8 text.\n"
        L"Folder input processes every *.db file in that folder.\n"
        L"Successful conversions replace the source file and remove its backup.\n";

    class WideStringWiper
    {
    public:
        explicit WideStringWiper(std::wstring& value)
            : value_(value)
        {
        }

        ~WideStringWiper()
        {
            if (!value_.empty())
            {
                SecureZeroMemory(
                    &value_[0], value_.size() * sizeof(wchar_t));
            }
        }

        WideStringWiper(const WideStringWiper&) = delete;
        WideStringWiper& operator=(const WideStringWiper&) = delete;

    private:
        std::wstring& value_;
    };

    class WideArgumentWiper
    {
    public:
        explicit WideArgumentWiper(wchar_t* value)
            : value_(value)
        {
        }

        ~WideArgumentWiper()
        {
            if (value_ != nullptr)
            {
                SecureZeroMemory(
                    value_, wcslen(value_) * sizeof(wchar_t));
            }
        }

        WideArgumentWiper(const WideArgumentWiper&) = delete;
        WideArgumentWiper& operator=(const WideArgumentWiper&) = delete;

    private:
        wchar_t* value_;
    };

    bool isHexDigit(wchar_t character)
    {
        return (character >= L'0' && character <= L'9') ||
            (character >= L'a' && character <= L'f') ||
            (character >= L'A' && character <= L'F');
    }

    wchar_t toUpperHex(wchar_t character)
    {
        if (character >= L'a' && character <= L'f')
        {
            return character - (L'a' - L'A');
        }
        return character;
    }

    bool parseKey(const std::wstring& input, std::string& hexKey, std::wstring& error)
    {
        std::string bytes;
        if (input.compare(0, 4, L"hex:") == 0)
        {
            std::wstring suppliedHex = input.substr(4);
            if (suppliedHex.empty() ||
                !std::all_of(suppliedHex.begin(), suppliedHex.end(), isHexDigit))
            {
                if (!suppliedHex.empty())
                {
                    SecureZeroMemory(
                        &suppliedHex[0], suppliedHex.size() * sizeof(wchar_t));
                }
                error = L"hex: key must contain a non-empty, even number of hexadecimal digits.";
                return false;
            }
            if (suppliedHex.size() % 2 != 0)
            {
                std::wostringstream message;
                message << L"hex: key has " << suppliedHex.size()
                        << L" hexadecimal digits; a byte key requires an even digit count.";
                error = message.str();
                SecureZeroMemory(
                    &suppliedHex[0], suppliedHex.size() * sizeof(wchar_t));
                return false;
            }

            bytes.reserve(suppliedHex.size());
            for (const wchar_t character : suppliedHex)
            {
                bytes.push_back(static_cast<char>(toUpperHex(character)));
            }
            SecureZeroMemory(
                &suppliedHex[0], suppliedHex.size() * sizeof(wchar_t));
        }
        else
        {
            bytes = toUtf8(input);
            if (bytes.empty())
            {
                error = L"Text key must not be empty or contain invalid Unicode.";
                return false;
            }

            static constexpr char HexDigits[] = "0123456789ABCDEF";
            std::string encodedHex;
            encodedHex.reserve(bytes.size() * 2);
            for (const unsigned char byte : bytes)
            {
                encodedHex.push_back(HexDigits[(byte >> 4) & 0x0F]);
                encodedHex.push_back(HexDigits[byte & 0x0F]);
            }
            secureClear(bytes);
            bytes = std::move(encodedHex);
            secureClear(encodedHex);
        }

        hexKey = std::move(bytes);
        secureClear(bytes);
        return true;
    }

    bool parseMode(const std::wstring& value, OperationMode& mode)
    {
        if (value == L"en")
        {
            mode = OperationMode::Encrypt;
            return true;
        }
        if (value == L"de")
        {
            mode = OperationMode::Decrypt;
            return true;
        }
        return false;
    }

    bool parseInputType(const std::wstring& value, InputType& inputType)
    {
        if (value == L"file")
        {
            inputType = InputType::File;
            return true;
        }
        if (value == L"folder")
        {
            inputType = InputType::Folder;
            return true;
        }
        return false;
    }

}

bool parseCommandLine(
    int argc,
    wchar_t* argv[],
    CliOptions& options,
    std::wstring& error)
{
    if (argc < 2)
    {
        error = Usage;
        return false;
    }

    if (!parseMode(argv[1], options.mode))
    {
        error = L"Invalid mode. Use 'en' or 'de'.\n";
        error += Usage;
        return false;
    }

    bool hasType = false;
    bool hasPath = false;
    bool hasKey = false;
    std::wstring keyInput;
    WideStringWiper keyInputWiper(keyInput);

    for (int index = 2; index < argc;)
    {
        const std::wstring option = argv[index];
        if (index + 1 >= argc)
        {
            error = L"Missing value for option: " + option;
            return false;
        }
        WideArgumentWiper argumentWiper(
            option == L"-k" ? argv[index + 1] : nullptr);
        std::wstring value = argv[index + 1];
        WideStringWiper valueWiper(value);
        if (option == L"-t")
        {
            if (hasType)
            {
                error = L"Duplicate option: -t";
                return false;
            }
            if (!parseInputType(value, options.inputType))
            {
                error = L"Input type must be 'file' or 'folder'.";
                return false;
            }
            hasType = true;
        }
        else if (option == L"-p")
        {
            if (hasPath)
            {
                error = L"Duplicate option: -p";
                return false;
            }
            options.path = value;
            hasPath = true;
        }
        else if (option == L"-k")
        {
            if (hasKey)
            {
                error = L"Duplicate option: -k";
                return false;
            }
            keyInput = value;
            hasKey = true;
        }
        else
        {
            error = L"Unknown option: " + option;
            return false;
        }
        index += 2;
    }

    if (!hasType || !hasPath || !hasKey)
    {
        error = L"Missing required options. Required: -t, -p, and -k.\n";
        error += Usage;
        return false;
    }
    if (options.path.empty())
    {
        error = L"Path must not be empty.";
        return false;
    }
    const bool keyParsed = parseKey(keyInput, options.hexKey, error);
    if (!keyParsed)
    {
        error = L"Invalid key: " + error;
        return false;
    }

    return true;
}

std::string toUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required == 0)
    {
        return {};
    }

    std::string result(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), &result[0], required,
            nullptr, nullptr) == 0)
    {
        return {};
    }
    return result;
}

void secureClear(std::string& value)
{
    if (!value.empty())
    {
        SecureZeroMemory(&value[0], value.size());
        value.clear();
    }
}
#include "database_files.h"
#include "encrypt_decrypt.h"
#include "utils.h"

#include <iostream>

int wmain(int argc, wchar_t* argv[])
{
    CliOptions options;
    std::wstring error;
    if (!parseCommandLine(argc, argv, options, error))
    {
        std::wcerr << error << L'\n';
        secureClear(options.hexKey);
        return 2;
    }

    if (!initializeQtRuntime(error))
    {
        std::wcerr << L"Could not initialize Qt: " << error << L'\n';
        secureClear(options.hexKey);
        clearQtRuntime();
        return 1;
    }

    const bool succeeded = processInputPath(
        options.inputType, options.path, options.mode, options.hexKey, error);
    if (!succeeded && !error.empty())
    {
        std::wcerr << L"Failed: " << error << L'\n';
    }

    secureClear(options.hexKey);
    clearQtRuntime();
    return succeeded ? 0 : 1;
}

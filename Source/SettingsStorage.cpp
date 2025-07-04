#include "SettingsStorage.h"
#include <cstring>
#include <format>
#include <sstream>

constexpr uint32_t SETTINGS_STORAGE_MUTEX_TIMEOUT_MS = 100;

// This operator overload allows the enum SettingPermissions_t to have a bitwise OR operator.
SettingPermissions_t operator|(SettingPermissions_t lhs, SettingPermissions_t rhs)
{
    return static_cast<SettingPermissions_t>(static_cast<std::byte>(lhs) | static_cast<std::byte>(rhs));
}

// This operator overload allows the enum SettingPermissions_t to have a bitwise AND operator.
SettingPermissions_t operator&(SettingPermissions_t lhs, SettingPermissions_t rhs)
{
    return static_cast<SettingPermissions_t>(static_cast<std::byte>(lhs) & static_cast<std::byte>(rhs));
}

// This function returns a formatted string of the permissions described in the parameter permission.
const char* settingPermissionToString(const SettingPermissions_t permission, char* permissionString,
                                      const size_t permissionStringSize)
{
    if (permissionString == nullptr || permissionStringSize < PERMISSION_STRING_SIZE ||
        !validatePermissions(permission))
    {
        return nullptr;
    }
    if (static_cast<bool>(permission & SettingPermissions_t::SYSTEM))
    {
        strncpy(permissionString, "SYSTEM | ", permissionStringSize);
    }
    else
    {
        strncpy(permissionString, "       | ", permissionStringSize);
    }
    if (static_cast<bool>(permission & SettingPermissions_t::ADMIN))
    {
        strncat(permissionString, "ADMIN | ", permissionStringSize);
    }
    else
    {
        strncat(permissionString, "      | ", permissionStringSize);
    }
    if (static_cast<bool>(permission & SettingPermissions_t::USER))
    {
        strncat(permissionString, "USER | ", permissionStringSize);
    }
    else
    {
        strncat(permissionString, "     | ", permissionStringSize);
    }
    if (static_cast<bool>(permission & SettingPermissions_t::VOLATILE))
    {
        strncat(permissionString, "VOLATILE", permissionStringSize);
    }
    else
    {
        strncat(permissionString, "        ", permissionStringSize);
    }
    return permissionString;
}

SettingsStorage::SettingsStorage(OSInterface& osInterface, SettingsFile* settingsFile)
{
    this->osInterface       = &osInterface;
    this->moduleConfigMutex = osInterface.osCreateMutex();
    assert(this->moduleConfigMutex != nullptr && "Mutex creation failed");

    this->persistentStorageEnabled = false;
    this->settings                 = new Settings_t(osInterface);

    this->settingsFile = settingsFile;
    if (settingsFile != nullptr)
    {
        this->persistentStorageEnabled = !CONFIG_SETTINGS_STORAGE_FORCE_DISABLE_PERSISTENT_STORAGE;
    }
}

SettingsStorage::~SettingsStorage()
{
    if (this->settingsFile != nullptr)
    {
        settingsFile->forceClose();
    }

    settings->iterateOverAll(freeSettingValuesCallback, nullptr);

    delete settings;
    delete moduleConfigMutex;
}

bool SettingsStorage::isPersistentStorageEnabled() const
{
    bool result = false;
    if (moduleConfigMutex->wait(SETTINGS_STORAGE_MUTEX_TIMEOUT_MS))
    {
        result = this->persistentStorageEnabled;
        moduleConfigMutex->signal();
    }
    return result;
}

bool SettingsStorage::disablePersistentStorage()
{
    bool res = false;
    if (moduleConfigMutex->wait(SETTINGS_STORAGE_MUTEX_TIMEOUT_MS))
    {
        this->persistentStorageEnabled = false;
        moduleConfigMutex->signal();
        res = true;
    }
    return res;
}

SettingsStorage::SettingError_t SettingsStorage::restoreDefaultSettings(const char*                    keyPrefix,
                                                                        SettingPermissions_t           permissions,
                                                                        SettingPermissionsFilterMode_t filterMode) const
{
    if (keyPrefix == nullptr)
    {
        return INVALID_INPUT_ERROR;
    }

    SettingsKeysList_t outputKeys;

    SettingError_t result = listSettingsKeys(keyPrefix, permissions, filterMode, outputKeys);
    if (result != NO_ERROR)
    {
        return result;
    }

    for (const auto& key : outputKeys)
    {
        SettingValue_t* outputValue;
        result = getSettingValue(key.c_str(), outputValue);
        if (result != NO_ERROR)
        {
            return result;
        }

        if (outputValue->settingValueType == STRING)
        {
            free(outputValue->settingValueData.string);
            outputValue->settingValueData.string = strdup(outputValue->settingDefaultValueData.string);
        }
        else
        {
            outputValue->settingValueData = outputValue->settingDefaultValueData;
        }
    }

    return NO_ERROR;
}

SettingsStorage::SettingError_t SettingsStorage::storeSettingsInPersistentStorage() const
{
    SettingsFile::SettingsFileResult res = settingsFile->openForWrite();
    if (res != SettingsFile::Success)
    {
        return SETTINGS_FILESYSTEM_ERROR;
    }

    uint32_t                    crc32;
    bool                        firstSetting = true;
    CRC::Table<unsigned, 32>    crcTable     = CRC::CRC_32().MakeTable();
    SettingsStoreCallbackData_t callbackData = std::make_tuple(settingsFile, &crc32, &firstSetting, &crcTable);
    res                                      = static_cast<SettingsFile::SettingsFileResult>(
        settings->iterateOverAll(storeSettingsInPersistentStorageCallback, &callbackData));
    if (res != SettingsFile::Success)
    {
        return SETTINGS_FILESYSTEM_ERROR;
    }

    std::string formattedString = std::format("\r{}\n", crc32);
    res                         = settingsFile->write(formattedString);
    if (res != SettingsFile::Success)
    {
        return SETTINGS_FILESYSTEM_ERROR;
    }

    res = settingsFile->close();
    if (res != SettingsFile::Success)
    {
        return SETTINGS_FILESYSTEM_ERROR;
    }
    return NO_ERROR;
}

SettingsStorage::SettingError_t SettingsStorage::validateChecksum() const
{
    uint32_t   expectedCrc32 = 0;
    uint32_t   computedCrc32 = 0;
    const auto crcTable      = CRC::CRC_32().MakeTable();

    SettingsFile::SettingsFileResult res = settingsFile->openForRead();
    if (res != SettingsFile::Success)
    {
        return SETTINGS_FILESYSTEM_ERROR;
    }

    bool firstSetting = true;
    while (res == SettingsFile::Success)
    {
        std::string settingStr;
        res = settingsFile->readLine(settingStr);

        if (res == SettingsFile::EndOfFile)
        {
            break;
        }

        if (settingStr[0] == '\r')
        {
            char* end;
            expectedCrc32 = static_cast<uint32_t>(std::strtol(&settingStr[1], &end, 10));
            if (*end != '\n')
            {
                settingsFile->close();
                return SETTINGS_FILESYSTEM_ERROR;
            }
        }
        else
        {
            if (firstSetting)
            {
                computedCrc32 = CRC::Calculate(settingStr.c_str(), settingStr.size(), crcTable);
                firstSetting  = false;
            }
            else
            {
                computedCrc32 = CRC::Calculate(settingStr.c_str(), settingStr.size(), crcTable, computedCrc32);
            }
        }
    }

    if (res != SettingsFile::EndOfFile)
    {
        settingsFile->close();
        return SETTINGS_FILESYSTEM_ERROR;
    }

    res = settingsFile->close();
    if (res != SettingsFile::Success)
    {
        return SETTINGS_FILESYSTEM_ERROR;
    }

    if (expectedCrc32 != computedCrc32)
    {
        return SETTINGS_FILESYSTEM_ERROR;
    }
    return NO_ERROR;
}

SettingsStorage::SettingError_t SettingsStorage::loadSettingsFromPersistentStorage() const
{
    if (const SettingError_t result = validateChecksum(); result != NO_ERROR)
    {
        return SETTINGS_FILESYSTEM_ERROR;
    }

    SettingsFile::SettingsFileResult res = settingsFile->openForRead();
    if (res != SettingsFile::Success)
    {
        return SETTINGS_FILESYSTEM_ERROR;
    }
    while (res == SettingsFile::Success)
    {
        std::string settingStr;
        res = settingsFile->readLine(settingStr);
        std::istringstream iss(settingStr);

        if (res == SettingsFile::EndOfFile)
        {
            break;
        }
        if (settingStr[0] == '\r')
        {
            continue;
        }

        std::string key;
        std::getline(iss, key, '\t');
        if (key.empty())
        {
            settingsFile->close();
            return SETTINGS_FILESYSTEM_ERROR;
        }

        std::string valueTypeStr;
        std::getline(iss, valueTypeStr, '\t');
        if (valueTypeStr.empty())
        {
            settingsFile->close();
            return SETTINGS_FILESYSTEM_ERROR;
        }

        std::string valueStr;
        std::getline(iss, valueStr, '\n');
        if (valueStr.empty())
        {
            settingsFile->close();
            return SETTINGS_FILESYSTEM_ERROR;
        }

        char* end;
        long  data = std::strtol(valueTypeStr.c_str(), &end, 10);
        if (*end != '\0' || data < 0 || data >= static_cast<uint8_t>(MAX_SETTING_VALUE_TYPE_ENUM))
        {
            settingsFile->close();
            return SETTINGS_FILESYSTEM_ERROR;
        }
        const auto     valueType = static_cast<SettingValueType_t>(data);
        SettingError_t settingError;

        switch (static_cast<uint8_t>(valueType))
        {
            case REAL:
            {
                const double realValue = std::strtod(valueStr.c_str(), &end);
                if (*end != '\0')
                {
                    settingsFile->close();
                    return SETTINGS_FILESYSTEM_ERROR;
                }

                settingError = putSettingValueAsReal(key.c_str(), realValue);

                if (settingError == KEY_NOT_FOUND_ERROR)
                {
                    settingError = registerSettingAsReal(key.c_str(), SettingPermissions_t::VOLATILE, realValue);
                    if (settingError != NO_ERROR)
                    {
                        settingsFile->close();
                        return SETTINGS_FILESYSTEM_ERROR;
                    }
                }
                else if (settingError != NO_ERROR)
                {
                    settingsFile->close();
                    return SETTINGS_FILESYSTEM_ERROR;
                }
            }
            break;
            case INTEGER:
            {
                const int64_t integerValue = std::strtoll(valueStr.c_str(), &end, 10);
                if (*end != '\0')
                {
                    settingsFile->close();
                    return SETTINGS_FILESYSTEM_ERROR;
                }

                settingError = putSettingValueAsInt(key.c_str(), integerValue);

                if (settingError == KEY_NOT_FOUND_ERROR)
                {
                    settingError = registerSettingAsInt(key.c_str(), SettingPermissions_t::VOLATILE, integerValue);
                    if (settingError != NO_ERROR)
                    {
                        settingsFile->close();
                        return SETTINGS_FILESYSTEM_ERROR;
                    }
                }
                else if (settingError != NO_ERROR)
                {
                    settingsFile->close();
                    return SETTINGS_FILESYSTEM_ERROR;
                }
            }
            break;
            case STRING:
            {
                settingError = putSettingValueAsString(key.c_str(), valueStr.c_str());

                if (settingError == KEY_NOT_FOUND_ERROR)
                {
                    settingError =
                        registerSettingAsString(key.c_str(), SettingPermissions_t::VOLATILE, valueStr.c_str());
                    if (settingError != NO_ERROR)
                    {
                        settingsFile->close();
                        return SETTINGS_FILESYSTEM_ERROR;
                    }
                }
                else if (settingError != NO_ERROR)
                {
                    settingsFile->close();
                    return SETTINGS_FILESYSTEM_ERROR;
                }
            }
            break;
            default:
                settingsFile->close();
                return SETTINGS_FILESYSTEM_ERROR;
        }
    }

    if (res != SettingsFile::EndOfFile)
    {
        settingsFile->close();
        return SETTINGS_FILESYSTEM_ERROR;
    }

    res = settingsFile->close();
    if (res != SettingsFile::Success)
    {
        return SETTINGS_FILESYSTEM_ERROR;
    }
    return NO_ERROR;
}

int SettingsStorage::listSettingsKeysCallback(void* data, const unsigned char* key, uint32_t key_len, void* value)
{
    auto*                          callbackData = static_cast<SettingsListCallbackData_t*>(data);
    SettingPermissions_t           permissions  = std::get<0>(*callbackData);
    SettingPermissionsFilterMode_t filterMode   = std::get<1>(*callbackData);
    SettingsKeysList_t*            outputKeys   = std::get<2>(*callbackData);
    auto*                          settingValue = static_cast<SettingValue_t*>(value);

    switch (filterMode)
    {
        case MatchSettingsWithAnyPermissionsListed:
        {
            if (static_cast<uint32_t>(settingValue->settingPermissions & permissions) >
                0) // If a bit is set in both, the result is greater than 0.
            {
                outputKeys->emplace_back(reinterpret_cast<const char*>(key), key_len);
            }
            return NO_ERROR;
        }

        case MatchSettingsWithAllPermissionsListed:
        {
            if (settingValue->settingPermissions == permissions)
            {
                outputKeys->emplace_back(reinterpret_cast<const char*>(key), key_len);
            }
            return NO_ERROR;
        }

        case ExcludeSettingsWithAllPermissionsListed:
        {
            if (settingValue->settingPermissions != permissions)
            {
                outputKeys->emplace_back(reinterpret_cast<const char*>(key), key_len);
            }
            return NO_ERROR;
        }

        case ExcludeSettingsWithAnyPermissionsListed:
        {
            if (static_cast<uint32_t>(settingValue->settingPermissions & permissions) == 0)
            {
                outputKeys->emplace_back(reinterpret_cast<const char*>(key), key_len);
            }
            return NO_ERROR;
        }
        default:
            return INVALID_INPUT_ERROR;
    }
}

int SettingsStorage::freeSettingValuesCallback([[maybe_unused]] void* data, [[maybe_unused]] const unsigned char* key,
                                               [[maybe_unused]] uint32_t key_len, void* value)
{
    auto* settingValue = static_cast<SettingValue_t*>(value);
    freeSettingValue(settingValue);

    return NO_ERROR;
}

int SettingsStorage::storeSettingsInPersistentStorageCallback(void* data, const unsigned char* key, uint32_t key_len,
                                                              void* value)
{
    auto* callbackData = static_cast<SettingsStoreCallbackData_t*>(data);
    auto* settingValue = static_cast<SettingValue_t*>(value);

    // If the setting is volatile, it should not be stored in the persistent storage.
    if (static_cast<bool>(settingValue->settingPermissions & SettingPermissions_t::VOLATILE))
    {
        return SettingsFile::Success;
    }

    SettingsFile*             settingsFile  = std::get<0>(*callbackData);
    uint32_t*                 settingsCRC32 = std::get<1>(*callbackData);
    bool*                     firstSetting  = std::get<2>(*callbackData);
    CRC::Table<unsigned, 32>* crcTable      = std::get<3>(*callbackData);

    if (*firstSetting)
    {
        *firstSetting  = false;
        *settingsCRC32 = CRC::Calculate(key, key_len, *crcTable);
    }
    else
    {
        *settingsCRC32 = CRC::Calculate(key, key_len, *crcTable, *settingsCRC32);
    }

    SettingsFile::SettingsFileResult res = settingsFile->write(reinterpret_cast<const char*>(key));
    if (res != SettingsFile::Success)
    {
        return res;
    }

    {
        const std::string formattedString = std::format("\t{}\t", static_cast<uint8_t>(settingValue->settingValueType));

        *settingsCRC32 = CRC::Calculate(formattedString.c_str(), formattedString.size(), *crcTable, *settingsCRC32);
        res            = settingsFile->write(formattedString);
        if (res != SettingsFile::Success)
        {
            return res;
        }
    }

    switch (settingValue->settingValueType)
    {
        case REAL:
        {
            std::string formattedString = std::format("{:.{}g}\n", settingValue->settingValueData.real,
                                                      std::numeric_limits<double>::max_digits10);

            *settingsCRC32 = CRC::Calculate(formattedString.c_str(), formattedString.size(), *crcTable, *settingsCRC32);
            res            = settingsFile->write(formattedString);
            break;
        }
        case INTEGER:
        {
            std::string formattedString = std::format("{}\n", settingValue->settingValueData.integer);

            *settingsCRC32 = CRC::Calculate(formattedString.c_str(), formattedString.size(), *crcTable, *settingsCRC32);
            res            = settingsFile->write(formattedString);
            break;
        }
        case STRING:
        {
            std::string formattedString = std::format("{}\n", settingValue->settingValueData.string);

            *settingsCRC32 = CRC::Calculate(formattedString.c_str(), formattedString.size(), *crcTable, *settingsCRC32);
            res            = settingsFile->write(formattedString);
            break;
        }
        default:
            res = SettingsFile::InvalidState;
    }
    return res;
}

SettingsStorage::SettingError_t SettingsStorage::listSettingsKeys(const char*                    keyPrefix,
                                                                  SettingPermissions_t           permissions,
                                                                  SettingPermissionsFilterMode_t filterMode,
                                                                  SettingsKeysList_t&            outputKeys) const
{
    if (keyPrefix == nullptr)
    {
        return INVALID_INPUT_ERROR;
    }

    if (!validatePermissions(permissions))
    {
        return INVALID_INPUT_ERROR;
    }

    SettingsListCallbackData_t callbackData = std::make_tuple(permissions, filterMode, &outputKeys);
    int res = settings->iterateOverPrefix(keyPrefix, static_cast<int>(strnlen(keyPrefix, MAX_SETTING_KEY_SIZE)),
                                          listSettingsKeysCallback, &callbackData);
    return static_cast<SettingError_t>(res);
}

SettingsStorage::SettingError_t SettingsStorage::getSettingAsInt(const char* key, int64_t& outputValue,
                                                                 SettingPermissions_t* outputPermissions) const
{
    return getSettingValueAsInt(Value, key, outputValue, outputPermissions);
}

SettingsStorage::SettingError_t SettingsStorage::getSettingAsReal(const char* key, double& outputValue,
                                                                  SettingPermissions_t* outputPermissions) const
{
    return getSettingValueAsReal(Value, key, outputValue, outputPermissions);
}

SettingsStorage::SettingError_t SettingsStorage::getSettingAsString(const char* key, char* outputValueBuffer,
                                                                    const size_t          outputValueSize,
                                                                    SettingPermissions_t* outputPermissions) const
{
    return getSettingValueAsString(Value, key, outputValueBuffer, outputValueSize, outputPermissions);
}

SettingsStorage::SettingError_t SettingsStorage::registerSettingAsInt(const char*                key,
                                                                      const SettingPermissions_t permissions,
                                                                      const int64_t              defaultValue) const
{
    if (key == nullptr || key[0] == '\0' || !validatePermissions(permissions))
    {
        return INVALID_INPUT_ERROR;
    }

    auto* newValue                            = new SettingValue_t();
    newValue->settingPermissions              = permissions;
    newValue->settingValueType                = INTEGER;
    newValue->settingValueData.integer        = defaultValue;
    newValue->settingDefaultValueData.integer = defaultValue;
    if (this->settings->insertIfNotExists(key, static_cast<int>(strnlen(key, MAX_SETTING_KEY_SIZE)), newValue) !=
        nullptr)
    {
        delete newValue;
        return KEY_EXISTS_ERROR;
    }
    return NO_ERROR;
}

SettingsStorage::SettingError_t SettingsStorage::registerSettingAsReal(const char*                key,
                                                                       const SettingPermissions_t permissions,
                                                                       const double               defaultValue) const
{
    if (key == nullptr || key[0] == '\0' || !validatePermissions(permissions))
    {
        return INVALID_INPUT_ERROR;
    }

    auto* newValue                         = new SettingValue_t();
    newValue->settingPermissions           = permissions;
    newValue->settingValueType             = REAL;
    newValue->settingValueData.real        = defaultValue;
    newValue->settingDefaultValueData.real = defaultValue;
    if (this->settings->insertIfNotExists(key, static_cast<int>(strnlen(key, MAX_SETTING_KEY_SIZE)), newValue) !=
        nullptr)
    {
        delete newValue;
        return KEY_EXISTS_ERROR;
    }
    return NO_ERROR;
}

SettingsStorage::SettingError_t SettingsStorage::registerSettingAsString(const char*                key,
                                                                         const SettingPermissions_t permissions,
                                                                         const char*                defaultValue) const
{
    if (key == nullptr || key[0] == '\0' || !validatePermissions(permissions) || defaultValue == nullptr)
    {
        return INVALID_INPUT_ERROR;
    }

    auto* newValue                           = new SettingValue_t();
    newValue->settingPermissions             = permissions;
    newValue->settingValueType               = STRING;
    newValue->settingValueData.string        = strdup(defaultValue);
    newValue->settingDefaultValueData.string = strdup(defaultValue);

    if (this->settings->insertIfNotExists(key, static_cast<int>(strnlen(key, MAX_SETTING_KEY_SIZE)), newValue) !=
        nullptr)
    {
        free(newValue->settingValueData.string);
        free(newValue->settingDefaultValueData.string);
        delete newValue;

        return KEY_EXISTS_ERROR;
    }
    return NO_ERROR;
}

SettingsStorage::SettingError_t SettingsStorage::putSettingValueAsInt(const char* key, const int64_t value) const
{
    SettingValue_t* outputValue;

    if (SettingError_t result = getSettingValue(key, outputValue); result != NO_ERROR)
    {
        return result;
    }

    if (outputValue->settingValueType != INTEGER)
    {
        return TYPE_MISMATCH_ERROR;
    }

    outputValue->settingValueData.integer = value;

    return NO_ERROR;
}

SettingsStorage::SettingError_t SettingsStorage::putSettingValueAsReal(const char* key, const double value) const
{
    SettingValue_t* outputValue;

    if (SettingError_t result = getSettingValue(key, outputValue); result != NO_ERROR)
    {
        return result;
    }

    if (outputValue->settingValueType != REAL)
    {
        return TYPE_MISMATCH_ERROR;
    }

    outputValue->settingValueData.real = value;

    return NO_ERROR;
}

SettingsStorage::SettingError_t SettingsStorage::putSettingValueAsString(const char* key, const char* value) const
{
    if (value == nullptr)
    {
        return INVALID_INPUT_ERROR;
    }

    SettingValue_t* outputValue;

    if (SettingError_t result = getSettingValue(key, outputValue); result != NO_ERROR)
    {
        return result;
    }

    if (outputValue->settingValueType != STRING)
    {
        return TYPE_MISMATCH_ERROR;
    }

    free(outputValue->settingValueData.string);
    outputValue->settingValueData.string = strdup(value);

    return NO_ERROR;
}

SettingsStorage::SettingError_t SettingsStorage::getDefaultSettingAsInt(const char* key, int64_t& outputValue,
                                                                        SettingPermissions_t* outputPermissions) const
{
    return getSettingValueAsInt(DefaultValue, key, outputValue, outputPermissions);
}

SettingsStorage::SettingError_t SettingsStorage::getDefaultSettingAsReal(const char* key, double& outputValue,
                                                                         SettingPermissions_t* outputPermissions) const
{
    return getSettingValueAsReal(DefaultValue, key, outputValue, outputPermissions);
}

SettingsStorage::SettingError_t
SettingsStorage::getDefaultSettingAsString(const char* key, char* outputValueBuffer, size_t outputValueSize,
                                           SettingPermissions_t* outputPermissions) const
{
    return getSettingValueAsString(DefaultValue, key, outputValueBuffer, outputValueSize, outputPermissions);
}

bool validatePermissions(const SettingPermissions_t permissions)
{
    return permissions <= ALL_PERMISSIONS_VOLATILE;
}

SettingsStorage::SettingError_t SettingsStorage::getSettingValue(const char* key, SettingValue_t*& outputValue) const
{
    if (key == nullptr || key[0] == '\0')
    {
        return INVALID_INPUT_ERROR;
    }

    outputValue = this->settings->search(key, static_cast<int>(strnlen(key, MAX_SETTING_KEY_SIZE)));
    if (outputValue == nullptr)
    {
        return KEY_NOT_FOUND_ERROR;
    }

    return NO_ERROR;
}

SettingsStorage::SettingError_t SettingsStorage::getSettingValueAsInt(TypeofSettingValue type, const char* key,
                                                                      int64_t&              outputValue,
                                                                      SettingPermissions_t* outputPermissions) const
{
    SettingValue_t* value;
    if (SettingError_t result = getSettingValue(key, value); result != NO_ERROR)
    {
        return result;
    }

    if (value->settingValueType != INTEGER)
    {
        return TYPE_MISMATCH_ERROR;
    }

    if (outputPermissions != nullptr)
    {
        *outputPermissions = value->settingPermissions;
    }

    outputValue = value->settingValueData.integer;
    if (type == DefaultValue)
    {
        outputValue = value->settingDefaultValueData.integer;
    }

    return NO_ERROR;
}

SettingsStorage::SettingError_t SettingsStorage::getSettingValueAsReal(TypeofSettingValue type, const char* key,
                                                                       double&               outputValue,
                                                                       SettingPermissions_t* outputPermissions) const
{
    SettingValue_t* value;
    if (SettingError_t result = getSettingValue(key, value); result != NO_ERROR)
    {
        return result;
    }

    if (value->settingValueType != REAL)
    {
        return TYPE_MISMATCH_ERROR;
    }

    if (outputPermissions != nullptr)
    {
        *outputPermissions = value->settingPermissions;
    }

    outputValue = value->settingValueData.real;
    if (type == DefaultValue)
    {
        outputValue = value->settingDefaultValueData.real;
    }

    return NO_ERROR;
}

SettingsStorage::SettingError_t SettingsStorage::getSettingValueAsString(const TypeofSettingValue type, const char* key,
                                                                         char*                 outputValueBuffer,
                                                                         const size_t          outputValueSize,
                                                                         SettingPermissions_t* outputPermissions) const
{
    if (outputValueBuffer == nullptr)
    {
        return INVALID_INPUT_ERROR;
    }

    SettingValue_t* value;
    if (SettingError_t result = getSettingValue(key, value); result != NO_ERROR)
    {
        return result;
    }

    if (value->settingValueType != STRING)
    {
        return TYPE_MISMATCH_ERROR;
    }

    const char* outputValue = value->settingValueData.string;
    if (type == DefaultValue)
    {
        outputValue = value->settingDefaultValueData.string;
    }

    if (strlen(outputValue) >= outputValueSize) // Only allow the string to be copied if it fits in the buffer. (The ==
                                                // is to account for the null terminator)
    {
        return INSUFFICIENT_BUFFER_SIZE_ERROR;
    }

    if (outputPermissions != nullptr)
    {
        *outputPermissions = value->settingPermissions;
    }
    strncpy(outputValueBuffer, outputValue, outputValueSize);
    outputValueBuffer[outputValueSize] = '\0';

    return NO_ERROR;
}

void SettingsStorage::freeSettingValue(const SettingValue_t* settingValue)
{
    if (settingValue->settingValueType == STRING)
    {
        free(settingValue->settingValueData.string);
        free(settingValue->settingDefaultValueData.string);
    }
    delete settingValue;
}

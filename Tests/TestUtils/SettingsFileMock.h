#ifndef SETTINGSFILEMOCK_H
#define SETTINGSFILEMOCK_H

#include "SettingsFile.h"
#include "cstdint"

class SettingsFileMock : public SettingsFile
{
public:
    explicit SettingsFileMock(const char* fileData, int64_t internalBufferSize = -1);
    ~SettingsFileMock() override;

    SettingsFileResult read(char* byte) override;

    SettingsFileResult readLine(std::string& buffer) override;

    SettingsFileResult write(char byte) override;

    SettingsFileResult write(const std::string& data) override;

    SettingsFileResult openForRead() override;

    SettingsFileResult openForWrite() override;

    SettingsFileResult close() override;

    void forceClose() override;

    FileStatus getOpenStatus() override;

    [[nodiscard]] char* _getInternalBuffer() const;

    void _setForceMockMode(bool fullMockEnabled);

    void _setReadResult(SettingsFileResult result);
    void _setReadOutput(char byte);

    void _setReadLineResult(SettingsFileResult result);
    void _setReadLineOutput(const char* line);

    void _setWriteResult(SettingsFileResult result);

    void _setWriteBufferResult(SettingsFileResult result);

    void _setOpenForReadResult(SettingsFileResult result);
    void _setOpenForWriteResult(SettingsFileResult result);
    void _setCloseResult(SettingsFileResult result);

private:
    char*      internalBuffer;
    uint32_t   fileDataSize;
    uint32_t   fileDataIndex;
    FileStatus fileStatus;
    uint32_t   internalBufferSize;

    bool               fullMockEnabled;
    SettingsFileResult readResult;
    char               readOutput;
    SettingsFileResult readLineResult;
    char               readLineOutput[1024]{};
    SettingsFileResult writeResult;
    SettingsFileResult writeBufferResult;
    SettingsFileResult openForReadResult;
    SettingsFileResult openForWriteResult;
    SettingsFileResult closeResult;
};

#endif // SETTINGSFILEMOCK_H

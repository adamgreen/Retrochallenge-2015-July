/* Copyright 2011 Adam Green

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
/* Acts as a server to satisfy DLOADM requests from a Tandy Color Computer.
*/
#include <windows.h>
#include <stdio.h>
#include <assert.h>


static void Usage()
{
    printf("Usage: dloadm ComPORT [--directory DirectoryName] [--baud ComBaudRate]\n"
           "Where:\n"
           "        ComPORT specifies the serial port to which the Color Computer is\n"
           "            connected.\n"
           "            Example: com2\n"
           "        --directory specifies the directory where the files to be hosted\n"
           "            up to the Color Computer are located on the PC.  These files\n"
           "            will be requested by the Color Computer when the user types\n"
           "            in DLOADM commands to the Extended Color BASIC interpreter.\n"
           "            Defaults to current directory.\n"
           "            NOTE: Filenames cannot be more than 8 characters long.\n"
           "        --baud sets the baud rate to be used for communicating with the Color\n"
           "            Computer.  It will default to 1200.\n"
           "\n"
           "Example Extended Color BASIC program to request a file named FOO at 1200 baud\n"
           "  DLOADM\"FOO\",1\n"
           "\n"
           "Another example which first sets the baud rate up to 2400 baud.\n"
           "  POKE &HE6,19\n"
           "  DLOADM\"FOO\"\n"
           "\n"
           "4800 baud example.\n"
           "  POKE &HE6,7\n"
           "  DLOADM\"FOO\"\n"
           );
}


typedef struct _SParameters
{
    const char*     pCOMPort;
    const char*     pDirectoryName;
    unsigned long   BaudRate;
} SParameters;

typedef struct _SServerState
{
    const char*     pDirectoryName;
    unsigned char*  pFileData;
    HANDLE          ComPortHandle;
    DWORD           FileSize;
} SServerState;


#define MACHINE_LANGUAGE    2
#define FILE_NOT_FOUND      0xFF
#define BINARY_ASCII_FLAG   0
#define INITIAL_CHECKSUM    0

#define FILE_OPEN_REQUEST_CODE  0x8A
#define BLOCK_REQUEST_CODE      0x97

#define COCO_BLOCK_SIZE     128


static BOOL ParseCommandLine(int argc, char* argv[], SParameters* pParameters)
{
    int i = 0;
    
    if (argc < 2)
    {
        fprintf(stderr, "error: COMPort parameter is required.\n\n");
        return FALSE;
    }
    
    pParameters->pCOMPort = argv[1];
    pParameters->pDirectoryName = NULL;
    pParameters->BaudRate = 1200;
    
    for (i = 2 ; i < argc ; i++)
    {
        if (0 == _stricmp(argv[i], "--baud"))
        {
            if (++i < argc)
            {
                pParameters->BaudRate = strtoul(argv[i], NULL, 0);
            }
            else
            {
                fprintf(stderr, "error: \"--baud\" must be followed by baud rate\n\n.");
                return FALSE;
            }            
        }
        else if (0 == _stricmp(argv[i], "--directory"))
        {
            if (++i < argc)
            {
                pParameters->pDirectoryName = argv[i];
            }
            else
            {
                fprintf(stderr, "error: \"--directory\" must be followed by directory name.\n\n");
                return FALSE;
            }            
        }
        else
        {
            fprintf(stderr, "error: %s isn't a recognized command.\n\n", argv[i]);
            return FALSE;
        }
    }

    return TRUE;
}


static HANDLE OpenComPort(SParameters* pParameters)
{
    int		Error = 1;
    HANDLE  Handle = INVALID_HANDLE_VALUE;
    DCB		dcb = { sizeof(DCB), 0 };
    BOOL	BoolReturn= FALSE;
    char    FullCOMPortName[MAX_PATH];

    sprintf_s(FullCOMPortName, sizeof(FullCOMPortName), "\\\\.\\%s", pParameters->pCOMPort);
    Handle = CreateFile(FullCOMPortName,
                        GENERIC_READ | GENERIC_WRITE,
                        0,
                        NULL,
                        OPEN_EXISTING,
                        0,
                        NULL );
    if (INVALID_HANDLE_VALUE == Handle)
    {
        goto Error;
    }

    BoolReturn = GetCommState(Handle, &dcb);
    if (!BoolReturn)
    {
        goto Error;
    }
    dcb.BaudRate            = pParameters->BaudRate;
    dcb.Parity              = NOPARITY;
    dcb.fParity				= FALSE;
    dcb.fOutxCtsFlow        = FALSE;
    dcb.fOutxDsrFlow        = FALSE;
    dcb.fDtrControl         = DTR_CONTROL_DISABLE;
    dcb.fDsrSensitivity     = FALSE;
    dcb.fTXContinueOnXoff   = TRUE;
    dcb.fOutX               = FALSE;
    dcb.fInX                = FALSE;
    dcb.fNull               = FALSE;
    dcb.fRtsControl         = RTS_CONTROL_DISABLE;
    dcb.ByteSize            = 8;
    dcb.StopBits            = ONESTOPBIT;
    dcb.fAbortOnError       = FALSE;

    BoolReturn = SetCommState(Handle, &dcb);
    if (!BoolReturn)
    {
        goto Error;
    }
    
    BoolReturn = PurgeComm(Handle, PURGE_TXCLEAR | PURGE_RXCLEAR);
    if (!BoolReturn)
    {
        goto Error;
    }

    Error = 0;
Error:
    if (Error && Handle!= INVALID_HANDLE_VALUE)
    {
        CloseHandle(Handle);
        Handle = INVALID_HANDLE_VALUE;
    }
    
    return	Handle;
}


static BOOL ReadFromCOMPortIntoBuffer(HANDLE ComPortHandle, void* pBuffer, unsigned int BufferSize)
{
    BOOL            Result = FALSE;
    unsigned int    BytesLeftToRead = BufferSize;
    unsigned char*  pCurrent = (unsigned char*)pBuffer;
    DWORD           ActualBytesRead = 0;
    
    while(BytesLeftToRead)
    {
        Result = ReadFile(ComPortHandle, pCurrent, BytesLeftToRead, &ActualBytesRead, NULL);
        if (!Result)
        {
            fprintf(stderr, "error: Failed to read data from CoCo. (%u)\n", GetLastError());
            return Result;
        }
        
        pCurrent += ActualBytesRead;
        BytesLeftToRead -= ActualBytesRead;
    }    
    
    return TRUE;
}


static BOOL WriteBufferToCOMPort(HANDLE ComPortHandle, const void* pBuffer, unsigned int BufferSize)
{
    BOOL                         Result = FALSE;
    DWORD                        ActualBytesSent = 0;
    
    Result = WriteFile(ComPortHandle, pBuffer, BufferSize, &ActualBytesSent, NULL);
    if (!Result || ActualBytesSent != BufferSize)
    {
        return FALSE;
    }
    
    return Result;
}


static BOOL AcknowledgeRequestCodeReceived(HANDLE ComPortHandle, unsigned char RequestCodeToAcknowledge)
{
    BOOL                         Result = FALSE;
    
    Result = WriteBufferToCOMPort(ComPortHandle, &RequestCodeToAcknowledge, sizeof(RequestCodeToAcknowledge));
    if (!Result)
    {
        fprintf(stderr, "error: Failed to send file request acknowledgement back to CoCo.\n");
    }
    
    return Result;
}


static BOOL AcknowledgeGoodChecksum(HANDLE ComPortHandle)
{
    static const unsigned char   FilenameAcknowledgeCode = 0xC8;
    BOOL                         Result = FALSE;
    
    Result = WriteBufferToCOMPort(ComPortHandle, &FilenameAcknowledgeCode, sizeof(FilenameAcknowledgeCode));
    if (!Result)
    {
        fprintf(stderr, "error: Failed to send acknowledgement back to CoCo.\n");
    }
    
    return Result;
}


static unsigned char CalculateChecksum(unsigned char* pBuffer, size_t BufferSize)
{
    unsigned char Checksum = 0;
    
    while (BufferSize-- > 0)
    {
        Checksum ^= *pBuffer++;
    }
    
    return Checksum;
}


static BOOL ValidateChecksum(unsigned char* pBuffer, size_t BufferSize)
{
    return (0 == CalculateChecksum(pBuffer, BufferSize));
}


static void RemoveTrailingWhitespace(unsigned char* pFilenameBuffer, size_t FilenameBufferSize)
{
    unsigned char* pCurrentCharacter;
    
    /* Very last character in filename buffer is the checksum so place a NULL terminator there incase all 8
       characters were used for the filename. */
    assert ( 9 == FilenameBufferSize );
    pCurrentCharacter = &pFilenameBuffer[8];
    *pCurrentCharacter = '\0';
    pCurrentCharacter--;
    
    while(' ' == *pCurrentCharacter && pCurrentCharacter >= pFilenameBuffer)
    {
        *pCurrentCharacter = '\0';
        pCurrentCharacter--;
    }
}


static BOOL ReceiveFilenameFromCoCo(HANDLE ComPortHandle, char* pFilenameBuffer, size_t FilenameBufferSize)
{
    BOOL            Result = FALSE;
    unsigned char   FilenameRequestWithChecksum[8+1];
    
    assert ( FilenameBufferSize >= 9 );
    
    Result = ReadFromCOMPortIntoBuffer(ComPortHandle, 
                                       FilenameRequestWithChecksum, 
                                       sizeof(FilenameRequestWithChecksum));
    if (!Result)
    {
        return Result;
    }
    
    if (!ValidateChecksum(FilenameRequestWithChecksum, sizeof(FilenameRequestWithChecksum)))
    {
        fprintf(stderr, "error: Filename received from CoCo failed checksum check.\n");
        return FALSE;
    }
    
    RemoveTrailingWhitespace(FilenameRequestWithChecksum, sizeof(FilenameRequestWithChecksum));
    strcpy(pFilenameBuffer, (const char*)FilenameRequestWithChecksum);
    
    return TRUE;
}


static BOOL FilenameIsBlank(const char* pFilename)
{
    return pFilename[0] == '\0';
}


static BOOL ConstructFullPathname(char*       pFullPathname, 
                                  size_t      FullPathnameSize, 
                                  const char* pFilename, 
                                  const char* pDirectoryName)
{
    if (FilenameIsBlank(pFilename))
    {
        fprintf(stderr, "error: Can't open blank filename on host.\n");
        return FALSE;
    }
    
    if (pDirectoryName)
    {
        _snprintf(pFullPathname, FullPathnameSize, "%s\\%s", pDirectoryName, pFilename);
    }
    else
    {
        strncpy(pFullPathname, pFilename, FullPathnameSize);
        pFullPathname[FullPathnameSize-1] = '\0';
    }
    
    return TRUE;
}


static void FreeFileData(SServerState* pServerState)
{
    pServerState->FileSize = 0;
    if (pServerState->pFileData)
    {
        free(pServerState->pFileData);
        pServerState->pFileData = NULL;
    }
}


static BOOL IsFileOpen(SServerState* pServerState)
{
    return (NULL != pServerState->pFileData);
}


static unsigned char* AllocateBufferForFile(HANDLE FileHandle, DWORD* pFileSize)
{
    DWORD   FileSize = 0;
    
    FileSize = GetFileSize(FileHandle, NULL);
    if (INVALID_FILE_SIZE == FileSize)
    {
        return NULL;
    }
    *pFileSize = FileSize;
    
    return malloc(FileSize);
}

static unsigned char* ReadFileIntoAllocatedBuffer(HANDLE FileHandle, DWORD* pFileSize)
{
    unsigned char* pReturn = NULL;
    unsigned char* pBuffer = NULL;
    DWORD          FileSize = 0;
    BOOL           Result = FALSE;
    DWORD          ActualBytesRead = 0;
    
    pBuffer = AllocateBufferForFile(FileHandle, &FileSize);
    if (!pBuffer)
    {
        fprintf(stderr, "error: Failed to allocate %u bytes to contain file contents.\n", FileSize);
        goto Error;
    }

    Result = ReadFile(FileHandle, pBuffer, FileSize, &ActualBytesRead, NULL);
    if (!Result || ActualBytesRead != FileSize)
    {
        fprintf(stderr, "error: Failed to read contents from host file. %u\n", GetLastError());
        goto Error;
    }

    pReturn = pBuffer;
    pBuffer = NULL;
    *pFileSize = FileSize;
Error:
    free(pBuffer);
    
    return pReturn;
}


static BOOL AttemptToOpenRequestedFile(SServerState* pServerState, const char* pFilename)
{
    BOOL    Result = FALSE;
    HANDLE  FileHandle = INVALID_HANDLE_VALUE;
    char    FullPathname[MAX_PATH];
    
    FreeFileData(pServerState);

    Result = ConstructFullPathname(FullPathname, sizeof(FullPathname), pFilename, pServerState->pDirectoryName);
    if (!Result)
    {
        return Result;
    }

    FileHandle = CreateFile(FullPathname,
                            GENERIC_READ,
                            FILE_SHARE_READ,
                            NULL,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL,
                            NULL);
    if (INVALID_HANDLE_VALUE == FileHandle)
    {
        return FALSE;
    }

    pServerState->pFileData = ReadFileIntoAllocatedBuffer(FileHandle, &pServerState->FileSize);

    CloseHandle(FileHandle);
    
    return IsFileOpen(pServerState);
}


static BOOL SendFileHeaderToCoCo(HANDLE ComPortHandle, BOOL FileIsFound)
{
    unsigned char              FileHeader[3] = { MACHINE_LANGUAGE, BINARY_ASCII_FLAG, INITIAL_CHECKSUM };
    BOOL                       Result = FALSE;
    
    if (!FileIsFound)
    {
        FileHeader[0] = FILE_NOT_FOUND;
    }
    
    FileHeader[2] = CalculateChecksum(FileHeader, 2);
    
    Result = WriteBufferToCOMPort(ComPortHandle, FileHeader, sizeof(FileHeader));
    if (!Result)
    {
        fprintf(stderr, "error: Failed to send file header to CoCo.\n");
    }
    
    return Result;
}


static BOOL ReceiveRequestCodeFromCoCo(HANDLE ComPortHandle, unsigned char* pRequestCode)
{
    BOOL            Result = FALSE;
    
    Result = ReadFromCOMPortIntoBuffer(ComPortHandle, pRequestCode, sizeof(*pRequestCode));
    if (!Result)
    {
        return Result;
    }

    return TRUE;
}


static BOOL ReceiveAndValidateBlockNumber(HANDLE ComPortHandle, unsigned short* pBlockNumber)
{
    BOOL            Result = FALSE;
    unsigned short  BlockNumber = 0;
    unsigned char   Buffer[3];
    
    Result = ReadFromCOMPortIntoBuffer(ComPortHandle, Buffer, sizeof(Buffer));
    if (!Result)
    {
        fprintf(stderr, "error: Failed to read block number from CoCo.\n");
        return Result;
    }

    if (!ValidateChecksum(Buffer, sizeof(Buffer)))
    {
        fprintf(stderr, "error: Checksum was invalid for block request from CoCo.\n");
        return FALSE;
    }

    /* The CoCo places the upper 7 bits of the block number in the first byte of the request and the lower 7 bits
       in the next byte. */
    BlockNumber = (unsigned short)Buffer[0] << 7;
    BlockNumber |= (unsigned short)Buffer[1];
    *pBlockNumber = BlockNumber;    
   
    return TRUE;
}


static unsigned int CalculateNumberOfBytesLeftInFile(unsigned int BlockStartingOffset, DWORD FileDataSize)
{
    if (BlockStartingOffset > FileDataSize)
    {
        return 0;
    }
    else
    {
        return (FileDataSize - BlockStartingOffset);
    }
}

static unsigned char CalculateValidByteCountForBlock(unsigned int BlockStartingOffset, DWORD FileDataSize)
{
    unsigned int    BytesLeftInFile = 0;
    unsigned char   BytesToSend = COCO_BLOCK_SIZE;
    
    BytesLeftInFile = CalculateNumberOfBytesLeftInFile(BlockStartingOffset, FileDataSize);

    if (BytesToSend > BytesLeftInFile)
    {
        BytesToSend = (unsigned char)BytesLeftInFile;
    }
    
    return BytesToSend;
}


static void FillBufferWithSpaces(unsigned char* pBuffer, size_t BufferSize)
{
    memset(pBuffer, ' ', BufferSize);
}


static void HandleFileOpenRequest(SServerState* pServerState)
{
    BOOL            Result = FALSE;
    BOOL            FileFoundResult = FALSE;
    char            FilenameBuffer[9] = "";
    HANDLE          ComPortHandle = pServerState->ComPortHandle;

    printf("\nHandling file open request...\n");
    
    FreeFileData(pServerState);
    
    Result = ReceiveFilenameFromCoCo(ComPortHandle, FilenameBuffer, sizeof(FilenameBuffer));
    if (!Result)
    {
        return;
    }
    printf("CoCo has requested the following file: %s\n", FilenameBuffer);
    
    Result = AcknowledgeGoodChecksum(ComPortHandle);
    if (!Result)
    {
        return;
    }
    
    FileFoundResult = AttemptToOpenRequestedFile(pServerState, FilenameBuffer);
    if (!FileFoundResult)
    {
        printf("File %s wasn't found on host.\n", FilenameBuffer);
    }
    
    Result = SendFileHeaderToCoCo(ComPortHandle, FileFoundResult);
    if (!Result)
    {
        return;
    }
    
    printf("File open request completed.\n");
}


static void HandleBlockRequest(SServerState* pServerState)
{
    BOOL            Result = FALSE;
    unsigned short  BlockNumber = 0;
    unsigned int    BlockStartingOffset = 0;
    unsigned char   ValidByteCount = 0;
    HANDLE          ComPortHandle = pServerState->ComPortHandle;
    unsigned char   BlockBuffer[1+COCO_BLOCK_SIZE+1];

    printf("\nHandling block request...\n");

    if (!IsFileOpen(pServerState))
    {
        fprintf(stderr, "error: Can't process block request since no file is currently open.\n");
        return;
    }
    
    Result = ReceiveAndValidateBlockNumber(ComPortHandle, &BlockNumber);
    if (!Result)
    {
        return;
    }
    
    Result = AcknowledgeGoodChecksum(ComPortHandle);
    if (!Result)
    {
        return;
    }
    
    BlockStartingOffset = BlockNumber * COCO_BLOCK_SIZE;
    ValidByteCount = CalculateValidByteCountForBlock(BlockStartingOffset, pServerState->FileSize);

    FillBufferWithSpaces(BlockBuffer, sizeof(BlockBuffer));
    BlockBuffer[0] = ValidByteCount;
    memcpy(BlockBuffer+1, pServerState->pFileData + BlockStartingOffset, ValidByteCount);
    BlockBuffer[1 + COCO_BLOCK_SIZE] = CalculateChecksum(BlockBuffer, sizeof(BlockBuffer) - 1);
    
    printf("Handling request for block #%u...\n", BlockNumber);
    printf("Sending %u valid bytes to CoCo for this block..\n", ValidByteCount);
    
    Result = WriteBufferToCOMPort(ComPortHandle, BlockBuffer, sizeof(BlockBuffer));
    if (!Result)
    {
        fprintf(stderr, "error: Failed to send block number %u to CoCo.\n", BlockNumber);
        return;
    }

    printf("Block request completed.\n");
}


static void HandleRequest(SServerState* pServerState, unsigned char RequestCode)
{
    switch(RequestCode)
    {
    case FILE_OPEN_REQUEST_CODE:
        HandleFileOpenRequest(pServerState);
        break;
    case BLOCK_REQUEST_CODE:
        HandleBlockRequest(pServerState);
        break;
    default:
        fprintf(stderr, "error: Received unrecognized request code of %02X from CoCo.\n", RequestCode);
    }
}


static void ProcessRequestsFromCoCo(SServerState* pServerState)
{
    HANDLE  ComPortHandle = pServerState->ComPortHandle;
    
    /* User must press CTRL-C to stop this app since it makes sync read cals to the COM port. */
    for (;;)
    {
        BOOL          Result;
        unsigned char RequestCode;
        
        Result = ReceiveRequestCodeFromCoCo(ComPortHandle, &RequestCode);
        if (!Result)
        {
            fprintf(stderr, "error: Failed to read in request code from CoCo. (%u)\n", GetLastError());
            return;
        }
        
        Result = AcknowledgeRequestCodeReceived(ComPortHandle, RequestCode);
        if (!Result)
        {
            fprintf(stderr, "error: Failed to acknowledge request code of %02X to CoCo.\n", RequestCode);
            return;
        }

        HandleRequest(pServerState, RequestCode);
    }
}


static void InitServerStateFields(SServerState* pServerState)
{
    pServerState->pDirectoryName = NULL;
    pServerState->pFileData = NULL;
    pServerState->ComPortHandle = INVALID_HANDLE_VALUE;
    pServerState->FileSize = 0;
}


static BOOL ConstructServerState(SServerState* pServerState, SParameters* pParameters)
{
    InitServerStateFields(pServerState);

    pServerState->pDirectoryName = pParameters->pDirectoryName;

    printf("Opening %s to CoCo with settings: %lu-8-N-1...\n", pParameters->pCOMPort, pParameters->BaudRate);
    pServerState->ComPortHandle = OpenComPort(pParameters);
    if (INVALID_HANDLE_VALUE == pServerState->ComPortHandle)
    {
        fprintf(stderr, "error: Failed to open %s (%u)\n", pParameters->pCOMPort, GetLastError());
        return FALSE;
    }

    return TRUE;
}


static void DestructServerState(SServerState* pServerState)
{
    FreeFileData(pServerState);

    if (INVALID_HANDLE_VALUE != pServerState->ComPortHandle)
    {
        CloseHandle(pServerState->ComPortHandle);
    }

    InitServerStateFields(pServerState);
}


int main(int argc, char* argv[])
{
    int                         Return = 1;
    BOOL                        Result = FALSE;
    SParameters                 Parameters;
    SServerState                ServerState;

    Result = ParseCommandLine(argc, argv, &Parameters);
    if (!Result)
    {
        Usage();
        goto Error;
    }

    Result = ConstructServerState(&ServerState, &Parameters);
    if (!Result)
    {
        goto Error;
    }
    
    printf("Press CTRL-C to exit this server application.\n");

    ProcessRequestsFromCoCo(&ServerState);

Error:
    DestructServerState(&ServerState);
    
    return Return;
}


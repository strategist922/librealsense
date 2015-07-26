#include <librealsense/R200/SPI.h>
#include <librealsense/R200/CalibIO.h>

using namespace std;

namespace r200
{

//@tofix ... this doesn't need to be global
// and all the free functions should actually live in a struct
// of some sort
uint32_t g_adminSectorAddresses[NV_ADMIN_DATA_N_ENTRIES];

////////////////////////
// SPI Free Functions //
////////////////////////

bool readPages(uvc_device_handle_t *devh, uint32_t address, unsigned char * buffer, uint32_t nPages)
{
    int addressTest = SPI_FLASH_TOTAL_SIZE_IN_BYTES - address - nPages * SPI_FLASH_PAGE_SIZE_IN_BYTES;
    
    if (!nPages || addressTest < 0)
        return false;
    
    // SendCommand()
    
    CommandPacket command;
    command.code = COMMAND_DOWNLOAD_SPI_FLASH;
    command.modifier = COMMAND_MODIFIER_DIRECT;
    command.tag = 12;
    command.address = address;
    command.value = nPages * SPI_FLASH_PAGE_SIZE_IN_BYTES;
    
    unsigned int cmdLength = sizeof(CommandPacket);
    auto XUWriteCmdStatus = uvc_set_ctrl(devh, CAMERA_XU_UNIT_ID, CONTROL_COMMAND_RESPONSE, &command, cmdLength);
    
    ResponsePacket response;
    unsigned int resLength = sizeof(ResponsePacket);
    auto XUReadCmdStatus = uvc_get_ctrl(devh, CAMERA_XU_UNIT_ID, CONTROL_COMMAND_RESPONSE, &response, resLength, UVC_GET_CUR);
    
    std::cout << "Read SPI Command Status: " << ResponseCodeToString(response.responseCode) << std::endl;
    
    // End SendCommand()
    
    bool returnStatus = true;
    
    if (XUReadCmdStatus > 0)
    {
        uint8_t *p = buffer;
        uint16_t spiLength = SPI_FLASH_PAGE_SIZE_IN_BYTES;
        for (unsigned int i = 0; i < nPages; ++i)
        {
            int bytesReturned = uvc_get_ctrl(devh, CAMERA_XU_UNIT_ID, CONTROL_COMMAND_RESPONSE, p, spiLength, UVC_GET_CUR);
            p += SPI_FLASH_PAGE_SIZE_IN_BYTES;
        }
    }
    
    return returnStatus;
}

void readArbitraryChunk(uvc_device_handle_t *devh, uint32_t address, void * dataIn, int lengthInBytesIn)
{
    unsigned char * data = (unsigned char *)dataIn;
    int lengthInBytes = lengthInBytesIn;
    unsigned char page[SPI_FLASH_PAGE_SIZE_IN_BYTES];
    int nPagesToRead;
    uint32_t startAddress = address;
    if (startAddress & 0xff)
    {
        // we are not on a page boundary
        startAddress = startAddress & ~0xff;
        uint32_t startInPage = address - startAddress;
        uint32_t lengthToCopy = SPI_FLASH_PAGE_SIZE_IN_BYTES - startInPage;
        if (lengthToCopy > (uint32_t)lengthInBytes)
            lengthToCopy = lengthInBytes;
        readPages(devh, startAddress, page, 1);
        memcpy(data, page + startInPage, lengthToCopy);
        lengthInBytes -= lengthToCopy;
        data += lengthToCopy;
        startAddress += SPI_FLASH_PAGE_SIZE_IN_BYTES;
    }
    nPagesToRead = lengthInBytes / SPI_FLASH_PAGE_SIZE_IN_BYTES;
    if (nPagesToRead > 0)
        readPages(devh, startAddress, data, nPagesToRead);
    lengthInBytes -= (nPagesToRead * SPI_FLASH_PAGE_SIZE_IN_BYTES);
    
    if (lengthInBytes)
    {
        // means we still have a remainder
        data += (nPagesToRead * SPI_FLASH_PAGE_SIZE_IN_BYTES);
        startAddress += (nPagesToRead * SPI_FLASH_PAGE_SIZE_IN_BYTES);
        readPages(devh, startAddress, page, 1);
        memcpy(data, page, lengthInBytes);
    }
}


bool readAdminSector(uvc_device_handle_t *devh, unsigned char data[SPI_FLASH_SECTOR_SIZE_IN_BYTES], int whichAdminSector)
{
    readArbitraryChunk(devh, NV_NON_FIRMWARE_ROOT_ADDRESS, g_adminSectorAddresses, NV_ADMIN_DATA_N_ENTRIES * sizeof(g_adminSectorAddresses[0]));
    
    if (whichAdminSector >= 0 && whichAdminSector < NV_ADMIN_DATA_N_ENTRIES)
    {
        uint32_t pageAddressInBytes = g_adminSectorAddresses[whichAdminSector];
        return readPages(devh, pageAddressInBytes, data, SPI_FLASH_PAGES_PER_SECTOR);
    }
    
    return false;
}

int getAdminSectorUsedCopies(uvc_device_handle_t *devh, uint32_t sectorAddress, uint32_t * unusedCopiesPtr)
{
    const uint32_t UNUSED_BITS_OFFSET = SPI_FLASH_SECTOR_SIZE_IN_BYTES - sizeof(uint32_t) - sizeof(uint32_t);
    
    uint32_t unusedCopies;
    uint32_t usedCopies;
    int usedCopiesCount = 0;
    uint32_t addressOfUnusedBits = sectorAddress + UNUSED_BITS_OFFSET;
    unsigned int i;
    
    readArbitraryChunk(devh, addressOfUnusedBits, &unusedCopies, sizeof(uint32_t));
    usedCopies = ~unusedCopies;
    
    for (i = 0; i < (sizeof(uint32_t) * 8); i++)
    {
        if (usedCopies & (1 << i)) usedCopiesCount++;
    }
    
    *unusedCopiesPtr = unusedCopies;
    return usedCopiesCount;
}

void readAdminTable(uvc_device_handle_t *devh, int whichAdminSector, int blockLength, void * data, int offset, int lengthToRead)
{
    uint32_t address = g_adminSectorAddresses[whichAdminSector];
    uint32_t dummy;
    int usedCopiesCount = getAdminSectorUsedCopies(devh, address, &dummy);
    uint32_t addressInSector = address + (usedCopiesCount - 1) * blockLength + offset;
    readArbitraryChunk(devh, addressInSector, data, lengthToRead);
}

///////////////////
// SPI Interface //
///////////////////

SPI_Interface::SPI_Interface(uvc_device_handle_t * devh) : deviceHandle(devh)
{
    
}

SPI_Interface::~SPI_Interface()
{
    
}

void SPI_Interface::Initialize()
{
    rst = {0};
    
    // Setup admin table
    readAdminTable(deviceHandle,
                   NV_IFFLEY_ROUTINE_TABLE_ADDRESS_INDEX,
                   SIZEOF_ROUTINE_DESCRIPTION_ERASED_AND_PRESERVE_TABLE,
                   rst.rd,
                   ROUTINE_DESCRIPTION_OFFSET,
                   SIZEOF_ROUTINE_DESCRIPTION_TABLE);
    
    ReadCalibrationSector();
}

void SPI_Interface::ReadCalibrationSector()
{
    if (!readAdminSector(deviceHandle, flashDataBuffer, NV_CALIBRATION_DATA_ADDRESS_INDEX))
        throw std::runtime_error("Could not read calibration sector");
    
    memcpy(calibrationDataBuffer, flashDataBuffer, CAM_INFO_BLOCK_LEN);
    memcpy(&cameraInfo, flashDataBuffer + CAM_INFO_BLOCK_LEN, sizeof(cameraInfo));
    
    cameraCalibration = ParseCalibrationParameters(calibrationDataBuffer);
}

void SPI_Interface::LogDebugInfo()
{
    std::cout << "#### Serial: " << cameraInfo.serialNumber << std::endl;
    std::cout << "#### Model: " << cameraInfo.modelNumber << std::endl;
    std::cout << "#### Revision: " << cameraInfo.revisionNumber << std::endl;
    std::cout << "#### Calibrated: " << cameraInfo.calibrationDate << std::endl;
    
    auto params = cameraCalibration.modesLR[0];
    
    std::cout << "## Mode LR[0] Rectified Intrinsics: " << std::endl;
    std::cout << "## Calibration Version: " << cameraCalibration.metadata.versionNumber << std::endl;
    std::cout << "## rfx: " << params.rfx << std::endl;
    std::cout << "## rfy: " << params.rfy << std::endl;
    std::cout << "## rpx: " << params.rpx << std::endl;
    std::cout << "## rpy: " << params.rpy << std::endl;
    std::cout << "## rw:  " << params.rw << std::endl;
    std::cout << "## rh:  " << params.rh << std::endl;
}

} // end namespace r200
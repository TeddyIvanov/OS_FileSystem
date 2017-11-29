#include <F17FS.h>
#include <block_store.h>
#include <bitmap.h>
#include <time.h>

#define BLOCK_STORE_NUM_BLOCKS 65536   // 2^16 blocks.
#define BLOCK_STORE_AVAIL_BLOCKS 65520 // Last 16 blocks consumed by the FBM
#define BLOCK_SIZE_BITS 4096         // 2^9 BYTES per block *2^3 BITS per BYTES
#define BLOCK_SIZE_BYTES 512         // 2^9 BYTES per block
#define BLOCK_STORE_NUM_BYTES (BLOCK_STORE_NUM_BLOCKS * BLOCK_SIZE_BYTES)  // 2^16 blocks of 2^9 bytes.

struct fileDescriptor{
    uint8_t inodeNumber;
    int filePosition;
};

struct inode{ //64 Bytes total
    int fileSize; //4 Bytes
    int deviceId; //4 Bytes
    int userId; //4 Bytes
    int groupId; //4 Bytes
    int fileMode; //4 Bytes
    int linkCount; //4 Bytes
    time_t changeTime; //8 Bytes
    time_t modifcationTime; //8 Bytes
    time_t accessTime; //8 Bytes
    uint16_t directBlocks[6]; //12 Bytes
    uint16_t indirectBlock; //2 Bytes
    uint16_t doubleIndirectBlock; //2 Bytes
};

struct directory{ //512 Bytes total
    file_record_t entries[7]; //7*65 bytes
    char metadata[57];
};

struct superRoot{
    //For Inodes
    bitmap_t* bitmap;
    size_t freeBlocks;
    size_t totalBlocks;
    size_t blockSize;
    uint8_t freeInodeMap[256];
    char metadata[512];
};

struct F17FS{
    block_store_t* blockStore;
    //For fileDescriptors
    bitmap_t* bitmap;
    fileDescriptor_t fds[256];
};
/// Formats (and mounts) an F17FS file for use
/// \param fname The file to format
/// \return Mounted F17FS object, NULL on error
F17FS_t *fs_format(const char *path){

    if(path == NULL || strcmp(path, "") == 0)
    {
        return NULL;
    }
    //Creating a blockstore from the given file.
    block_store_t* blockStore = block_store_create(path);
    //Requesting allocated space for the 0th block in the blockstore to store root.
    if(!block_store_request(blockStore, 0)){
        return NULL;
    }
    //Creating the superRoot that will be placed in the first block in the blockstore.
    superRoot_t* root = calloc(1, sizeof(superRoot_t));
    //Bitmap to keep track of the 32 inodes.
    root->bitmap = bitmap_overlay(256, root->freeInodeMap);
    //Marking the 0th block in the blockStore in use because the root is in there.
    bitmap_set(root->bitmap, 0);
    root->blockSize = BLOCK_SIZE_BYTES;
    root->freeBlocks = block_store_get_free_blocks(blockStore);
    root->totalBlocks = block_store_get_total_blocks();
    //Writing the root to the blockStore.
    block_store_write(blockStore, 0, root);

    //Initializing the inodes stored in beginning 32 blocks after the superRoot block.
    size_t i = 0;
    for(i = 0; i<32; i++){
        inode_t* inode = calloc(8, sizeof(inode_t));
        //Asking for the space in the blockstore.
        size_t blockId = block_store_allocate(blockStore);
        block_store_write(blockStore, blockId, inode);
        free(inode);
    }

    //Creating root directory so, need first inode in the blockStore.
    inode_t* inode = calloc(8, sizeof(inode_t));
    block_store_read(blockStore, 1, inode);
    //Initializing basic parts for root, might need more.
    inode[0].fileSize = sizeof(directory_t);
    inode[0].fileMode = 1777; //Permissions
    inode[0].accessTime = time(0);
    inode[0].changeTime = time(0);
    inode[0].modifcationTime = time(0);
    //Finding a free block to put the directory.
    size_t blockId = block_store_allocate(blockStore);
    inode[0].directBlocks[0] = (uint8_t)blockId;

    //Updating the inode in the blockstore.
    block_store_write(blockStore, 1, inode);

    //Writing the directory into that found free block.
    directory_t* directory = calloc(1, sizeof(directory_t));
    block_store_write(blockStore, blockId, directory);

    //Clean up my allocations
    block_store_destroy(blockStore);
    bitmap_destroy(root->bitmap);
    free(root);
    free(inode);
    free(directory);

    //Mounting the filesystem now.
    F17FS_t* fileSystem = fs_mount(path);
    return fileSystem;
}
/// Mounts an F17FS object and prepares it for use
/// \param fname The file to mount
/// \return Mounted F17FS object, NULL on error
F17FS_t *fs_mount(const char *path){

    if(path == NULL || strcmp(path, "") == 0){
        return NULL;
    }
    block_store_t* blockStore = block_store_open(path);
    if(blockStore == NULL) {
        return NULL;
    }

    F17FS_t* fileSystem = calloc(1, sizeof(F17FS_t));
    fileSystem->blockStore = blockStore;
    fileSystem->bitmap = bitmap_create(BLOCK_SIZE_BYTES/2);

    return fileSystem;
}
/// Unmounts the given object and frees all related resources
/// \param fs The F17FS object to unmount
/// \return 0 on success, < 0 on failure
int fs_unmount(F17FS_t *fs){
    if(fs == NULL)
    {
        return -1;
    }else if(fs->blockStore == NULL && fs->bitmap == NULL){
        return -1;
    }else {
        block_store_destroy(fs->blockStore);
        bitmap_destroy(fs->bitmap);
        free(fs);
        return 0;
    }
}
/// Creates a new file at the specified location
///   Directories along the path that do not exist are not created
/// \param fs The F17FS containing the file
/// \param path Absolute path to file to create
/// \param type Type of file to create (regular/directory)
/// \return 0 on success, < 0 on failure

int fs_create(F17FS_t *fs, const char *path, file_t type) {

    //Error check file path for Null.
    if(fs == NULL || path == NULL || strcmp(path, "") == 0 || (type != FS_REGULAR && type != FS_DIRECTORY)) {
      return -1;
    }

    inode_t* inodeForParent = calloc(1, sizeof(inode_t));
    directory_t* parentDirectory = calloc(1, sizeof(directory_t));
    file_record_t* file = calloc(1, sizeof(file_record_t));
    //Traverse directory structure
    int succesfullyTraversed = traverseFilePath(path, fs, parentDirectory, inodeForParent, file);
    if(succesfullyTraversed < 0){
        free(file);
        free(parentDirectory);
        free(inodeForParent);
        return -1;
    }

    int validSpaceToCreate = checkBlockInDirectory(parentDirectory, file);
    if (validSpaceToCreate < 0){
        free(inodeForParent);
        free(parentDirectory);
        free(file);
        return -1;
    }
    //Getting my root for checking Inodes
    superRoot_t* root = calloc(1, sizeof(superRoot_t));
    block_store_read(fs->blockStore, 0, root);
    root->bitmap = bitmap_overlay(256, root->freeInodeMap);
    size_t inodeNumberInInodeTable = bitmap_ffz(root->bitmap);
    if(inodeNumberInInodeTable == SIZE_MAX){
        bitmap_destroy(root->bitmap);
        free(inodeForParent);
        free(parentDirectory);
        free(file);
        free(root);
        return -1;
    }
    inode_t* inodeForDirectoryOrFile = calloc(1, sizeof(inode_t));
    getInodeFromTable(fs,inodeNumberInInodeTable, inodeForDirectoryOrFile);
    if(type == FS_DIRECTORY){
        size_t freeDataBlockId = block_store_allocate(fs->blockStore);
        if(freeDataBlockId == SIZE_MAX){
            bitmap_destroy(root->bitmap);
            free(inodeForParent);
            free(parentDirectory);
            free(file);
            free(root);
            free(inodeForDirectoryOrFile);
            return -1;
        }
        directory_t* requestedNewDirectory = calloc(1, sizeof(directory_t));
        inodeForDirectoryOrFile[0].fileSize = sizeof(directory_t);
        inodeForDirectoryOrFile[0].fileMode = 1777; //Permissions
        inodeForDirectoryOrFile[0].accessTime = time(0);
        inodeForDirectoryOrFile[0].changeTime = time(0);
        inodeForDirectoryOrFile[0].modifcationTime = time(0);
        inodeForDirectoryOrFile[0].directBlocks[0] = (uint8_t)freeDataBlockId;
        //Writing directory into the directBlock.
        block_store_write(fs->blockStore, freeDataBlockId, requestedNewDirectory);
        //Writing the Inode back into the Inode Table.
        writeInodeIntoTable(fs,inodeNumberInInodeTable, inodeForDirectoryOrFile);
        //Updating the parentDirectory and writing it to back to disc.
        parentDirectory->entries[validSpaceToCreate].inodeNumber = (uint8_t)inodeNumberInInodeTable;
        strcpy(parentDirectory->entries[validSpaceToCreate].name, file->name);
        parentDirectory->entries[validSpaceToCreate].type = FS_DIRECTORY;
        block_store_write(fs->blockStore, inodeForParent->directBlocks[0], parentDirectory);
        free(requestedNewDirectory);
    }else{
        inodeForDirectoryOrFile[0].fileSize = 0;
        inodeForDirectoryOrFile[0].fileMode = 777; //Permissions
        inodeForDirectoryOrFile[0].accessTime = time(0);
        inodeForDirectoryOrFile[0].changeTime = time(0);
        inodeForDirectoryOrFile[0].modifcationTime = time(0);
        //Writing the Inode back into the Inode Table.
        writeInodeIntoTable(fs,inodeNumberInInodeTable, inodeForDirectoryOrFile);
        //Updating the parentDirectory and writing it to back to disc.
        parentDirectory->entries[validSpaceToCreate].inodeNumber = (uint8_t)inodeNumberInInodeTable;
        strcpy(parentDirectory->entries[validSpaceToCreate].name, file->name);
        parentDirectory->entries[validSpaceToCreate].type = FS_REGULAR;
        block_store_write(fs->blockStore, inodeForParent->directBlocks[0], parentDirectory);
    }
    //Updating the root after creating a file or directory.
    bitmap_set(root->bitmap, inodeNumberInInodeTable);
    block_store_write(fs->blockStore, 0, root);

    //CleanUp!
    bitmap_destroy(root->bitmap);
    free(inodeForParent);
    free(parentDirectory);
    free(file);
    free(root);
    free(inodeForDirectoryOrFile);

    return 0;
}

/// Opens the specified file for use
///   R/W position is set to the beginning of the file (BOF)
///   Directories cannot be opened
/// \param fs The F17FS containing the file
/// \param path path to the requested file
/// \return file descriptor to the requested file, < 0 on error
int fs_open(F17FS_t *fs, const char *path) {
    if(fs == NULL || path == NULL || strcmp(path, "") == 0){
        return -1;
    }
    inode_t* inodeForParent = calloc(1, sizeof(inode_t));
    directory_t* parentDirectory = calloc(1, sizeof(directory_t));
    file_record_t* file = calloc(1, sizeof(file_record_t));
    //Traverse directory structure
    int succesfullyTraversed = traverseFilePath(path, fs, parentDirectory, inodeForParent, file);
    //Check to see if it was found.
    if(succesfullyTraversed < 0){
        free(file);
        free(parentDirectory);
        free(inodeForParent);
        return -1;
    }
    //Check to see if its in the directory entries.
    int fileLocation = indexOfNameInDirectoryEntries(*parentDirectory, file->name);
    if(fileLocation < 0){
        free(inodeForParent);
        free(parentDirectory);
        free(file);
        return -1;
    }
    //Check to see if there is enough fileDescriptors
    size_t indexOfFileDescriptor = bitmap_ffz(fs->bitmap);
    if(indexOfFileDescriptor == SIZE_MAX){
        free(inodeForParent);
        free(parentDirectory);
        free(file);
        return -1;
    }
    //Check to see if its directory.
    if(parentDirectory->entries[fileLocation].type == FS_DIRECTORY){
        free(inodeForParent);
        free(parentDirectory);
        free(file);
        return -1;
    }
    //Updating Filedescriptor to being in use.
    bitmap_set(fs->bitmap, indexOfFileDescriptor);
    fs->fds[indexOfFileDescriptor].inodeNumber = parentDirectory->entries[fileLocation].inodeNumber;
    fs->fds[indexOfFileDescriptor].filePosition = 0;
    //Cleanup
    free(inodeForParent);
    free(parentDirectory);
    free(file);
    return (int)indexOfFileDescriptor;
}

/// Closes the given file descriptor
/// \param fs The F17FS containing the file
/// \param fd The file to close
/// \return 0 on success, < 0 on failure
int fs_close(F17FS_t *fs, int fd){
    if(fs == NULL || fd <0 || fd > 255){
        return -1;
    }
    //Checking to is if it was actually in us.
    if(!bitmap_test(fs->bitmap, (size_t)fd)){
        return -1;
    }
    //Resetting it.
    bitmap_reset(fs->bitmap, (size_t)fd);
    fs->fds[fd].filePosition = 0;
    fs->fds[fd].inodeNumber = '\0';
    return 0;
}
///
/// Populates a dyn_array with information about the files in a directory
///   Array contains up to 15 file_record_t structures
/// \param fs The F17FS containing the file
/// \param path Absolute path to the directory to inspect
/// \return dyn_array of file records, NULL on error
///
dyn_array_t *fs_get_dir(F17FS_t *fs, const char *path){
    if(fs == NULL || path == NULL || strcmp(path, "") == 0){
        return NULL;
    }
    inode_t* inodeForParent = calloc(1, sizeof(inode_t));
    directory_t* parentDirectory = calloc(1, sizeof(directory_t));
    file_record_t* file = calloc(1, sizeof(file_record_t));
    dyn_array_t* dynArray = dyn_array_create(7, sizeof(file_record_t), NULL);
    //Traverse directory structure
    if(strlen(path) == 1 && path[0] == '/'){
        //Getting root
        inode_t* blockSizeOfInodes = calloc(8, sizeof(inode_t));
        block_store_read(fs->blockStore, 1 , blockSizeOfInodes);
        //Getting the first Inode.
        //inodeForParent = &blockSizeOfInodes[0];
        memcpy(inodeForParent,&blockSizeOfInodes[0], sizeof(inode_t));
        //Getting the root directory from the dataBlocks of the first inode.
        block_store_read(fs->blockStore, inodeForParent->directBlocks[0], parentDirectory);
        int i;
        for(i = 0; i<7; i++){
            if(parentDirectory->entries[i].inodeNumber != '\0'){
                dyn_array_push_front(dynArray, &parentDirectory->entries[i]);
            }
        }
        free(inodeForParent);
        free(parentDirectory);
        free(file);
        free(blockSizeOfInodes);
        return dynArray;
    }

    int succesfullyTraversed = traverseFilePath(path, fs, parentDirectory, inodeForParent, file);
    //Check to see if it was found.
    if(succesfullyTraversed < 0){
        free(file);
        free(parentDirectory);
        free(inodeForParent);
        dyn_array_destroy(dynArray);
        return NULL;
    }

    //Check to see if fileLocation exists.
    int fileLocation = indexOfNameInDirectoryEntries(*parentDirectory, file->name);
    if(fileLocation < 0){
        free(inodeForParent);
        free(parentDirectory);
        free(file);
        dyn_array_destroy(dynArray);
        return NULL;
    }
    //Check if its a file and return.
    if(parentDirectory->entries[fileLocation].type == FS_REGULAR){
        free(inodeForParent);
        free(parentDirectory);
        free(file);
        dyn_array_destroy(dynArray);
        return NULL;
    }
    //Getting Inode and getting the parent directory.
    getInodeFromTable(fs,parentDirectory->entries[fileLocation].inodeNumber, inodeForParent);
    block_store_read(fs->blockStore, inodeForParent->directBlocks[0], parentDirectory);
    //Looping for all directories and adding them to the dynamic array.
    int i;
    for(i = 0; i<7; i++){
        if(parentDirectory->entries[i].inodeNumber != '\0'){
            dyn_array_push_front(dynArray, &parentDirectory->entries[i]);
        }
    }
    //CleanUp
    free(inodeForParent);
    free(parentDirectory);
    free(file);

    return dynArray;
}

/// Moves the R/W position of the given descriptor to the given location
///   Files cannot be seeked past EOF or before BOF (beginning of file)
///   Seeking past EOF will seek to EOF, seeking before BOF will seek to BOF
/// \param fs The F17FS containing the file
/// \param fd The descriptor to seek
/// \param offset Desired offset relative to whence
/// \param whence Position from which offset is applied
/// \return offset from BOF, < 0 on error
off_t fs_seek(F17FS_t *fs, int fd, off_t offset, seek_t whence){

    if(fs == NULL){
        return -1;
    }

    if(fd < 0 || fd > 255){
        return -1;
    }
    //See if its in use.
    if(!bitmap_test(fs->bitmap, fd)){
        return -1;
    }
    uint8_t inodeLocation = fs->fds[fd].inodeNumber;
    inode_t* fileInode = calloc(1, sizeof(inode_t));
    getInodeFromTable(fs, inodeLocation, fileInode);
    int fileSize = fileInode->fileSize;

    free(fileInode);
    int currentFilePosition =0;
    off_t seekLocation = 0;
    switch (whence) {
        case FS_SEEK_SET:
            seekLocation = calculateOffset(fileSize, offset);
            fs->fds[fd].filePosition = seekLocation;
            return seekLocation;

        case FS_SEEK_CUR:
            currentFilePosition = fs->fds[fd].filePosition;
            seekLocation = currentFilePosition + offset;
            seekLocation = calculateOffset(fileSize, seekLocation);
            fs->fds[fd].filePosition = seekLocation;
            return seekLocation;

        case FS_SEEK_END:
            seekLocation = fileSize + offset;
            seekLocation = calculateOffset(fileSize, seekLocation);
            fs->fds[fd].filePosition = seekLocation;
            return seekLocation;

        default:
            return -1;
    }
}


/// Reads data from the file linked to the given descriptor
///   Reading past EOF returns data up to EOF
///   R/W position in incremented by the number of bytes read
/// \param fs The F17FS containing the file
/// \param fd The file to read from
/// \param dst The buffer to write to
/// \param nbyte The number of bytes to read
/// \return number of bytes read (< nbyte IFF read passes EOF), < 0 on error
ssize_t fs_read(F17FS_t *fs, int fd, void *dst, size_t nbyte){

    if(fs == NULL || dst == NULL) {
        return -1;
    }
    if(fd < 0 || fd > 255){
        return -1;
    }
    if(nbyte == 0){
        return 0;
    }
    if(!bitmap_test(fs->bitmap, fd)){
        return -1;
    }

    uint8_t inodeLocation = fs->fds[fd].inodeNumber;
    inode_t* fileInode = calloc(1, sizeof(inode_t));
    getInodeFromTable(fs,inodeLocation, fileInode);
    ssize_t totalBytesRead = 0;
    size_t requestedReadAmount = nbyte;

    if((int)(requestedReadAmount+fs->fds[fd].filePosition) > fileInode->fileSize){
        requestedReadAmount = fileInode->fileSize - fs->fds[fd].filePosition;
    }

    int currentFilePosition = fs->fds[fd].filePosition;
    int fileBlockNumber = currentFilePosition/BLOCK_SIZE_BYTES;
    //Where you are in fileBlock.
    int byteAtPositionInFileBlock = currentFilePosition % BLOCK_SIZE_BYTES;

    if(fileBlockNumber < 6){
        totalBytesRead = readDirectBlocks(fs->blockStore, fileBlockNumber, byteAtPositionInFileBlock, fileInode, dst, requestedReadAmount);
    }else if (fileBlockNumber >= 6 && fileBlockNumber <= 261){
        totalBytesRead = readIndirectBlock(fs->blockStore, fileBlockNumber, byteAtPositionInFileBlock, fileInode, dst, requestedReadAmount, fileInode->indirectBlock);
    }else {
        totalBytesRead = readDoubleIndirectBlocks(fs->blockStore, fileBlockNumber, byteAtPositionInFileBlock, fileInode, dst, requestedReadAmount);
    }

    fs->fds[fd].filePosition += totalBytesRead;
    free(fileInode);

    return totalBytesRead;
}

///
/// Writes data from given buffer to the file linked to the descriptor
///   Writing past EOF extends the file
///   Writing inside a file overwrites existing data
///   R/W position in incremented by the number of bytes written
/// \param fs The F17FS containing the file
/// \param fd The file to write to
/// \param dst The buffer to read from
/// \param nbyte The number of bytes to write
/// \return number of bytes written (< nbyte IFF out of space), < 0 on error
///
ssize_t fs_write(F17FS_t *fs, int fd, const void *src, size_t nbyte){
    if(fs == NULL || src == NULL) {
        return -1;
    }
    if(fd < 0 || fd > 255){
        return -1;
    }
    if(nbyte == 0){
        return 0;
    }
    if(!bitmap_test(fs->bitmap, fd)){
        return -1;
    }
    uint8_t inodeLocation = fs->fds[fd].inodeNumber;
    inode_t* fileInode = calloc(1, sizeof(inode_t));
    getInodeFromTable(fs,inodeLocation, fileInode);
    ssize_t totalBytesWritten = 0;
    size_t physicalBlock = 0;

    int currentFilePosition = fs->fds[fd].filePosition;
    int fileBlockNumber = currentFilePosition/BLOCK_SIZE_BYTES;
    //Where you are in fileBlock.
    int byteAtPositionInFileBlock = currentFilePosition % BLOCK_SIZE_BYTES;


    if(fileBlockNumber < 6){
        totalBytesWritten = handleDirectBlocks(fs->blockStore, fileBlockNumber, byteAtPositionInFileBlock, fileInode, src, nbyte);
    }else if (fileBlockNumber >= 6 && fileBlockNumber < 261) {

        uint16_t readIndirectData[256];
        physicalBlock = fileInode->indirectBlock;

        if (physicalBlock == 0) {
            physicalBlock = block_store_allocate(fs->blockStore);
            //Making sure I don't run out of space.
            if(physicalBlock == SIZE_MAX){
                return 0;
            }
            fileInode->indirectBlock = (uint16_t) physicalBlock;
            memset(readIndirectData, 0, 512);
            block_store_write(fs->blockStore, physicalBlock, readIndirectData);
        }
        totalBytesWritten = handleIndirectBlock(fs->blockStore, fileBlockNumber, byteAtPositionInFileBlock, fileInode, src, nbyte, physicalBlock);
    }else{
        totalBytesWritten = handleDoubleIndirectBlocks(fs->blockStore,fileBlockNumber,byteAtPositionInFileBlock, fileInode, src, nbyte);
    }

    fs->fds[fd].filePosition += totalBytesWritten;
    fileInode->fileSize += totalBytesWritten;
    writeInodeIntoTable(fs, fs->fds[fd].inodeNumber, fileInode);
    free(fileInode);

    return totalBytesWritten;
}

ssize_t readDirectBlocks(block_store_t* blockStore, int fileBlockNumber, int byteAtPositionInFileBlock, inode_t* inode, char* data, size_t nbytes){
    size_t physicalBlock = inode->directBlocks[fileBlockNumber];
    if(physicalBlock == 0){
        return 0;
    }
    size_t bytesToReadFromLocation = (size_t)512 - byteAtPositionInFileBlock;
    if(bytesToReadFromLocation > nbytes){
        bytesToReadFromLocation = nbytes;
    }
    char readDataBlock[512];
    block_store_read(blockStore, physicalBlock, readDataBlock);
    memcpy(data, (readDataBlock + byteAtPositionInFileBlock), bytesToReadFromLocation);
    nbytes -= bytesToReadFromLocation;
    if(nbytes > 0 && fileBlockNumber + 1 < 6){
        return bytesToReadFromLocation + readDirectBlocks(blockStore, fileBlockNumber + 1, 0, inode, (data+bytesToReadFromLocation), nbytes);
    } else if (nbytes > 0 && fileBlockNumber + 1 >= 6){
        return bytesToReadFromLocation + readIndirectBlock(blockStore, fileBlockNumber + 1, 0, inode, (data + bytesToReadFromLocation), nbytes, inode->indirectBlock);
    }else {
        return bytesToReadFromLocation;
    }
}
//Direct Block
ssize_t handleDirectBlocks(block_store_t* blockStore, int fileBlockNumber, int byteAtPositionInFileBlock, inode_t* inode, const char* data, size_t nbytes){

    size_t physicalBlock = 0;
    bool needBlock = false;
    if(inode->directBlocks[fileBlockNumber] == 0){
        physicalBlock = block_store_allocate(blockStore);
        if(physicalBlock == SIZE_MAX){
            return 0;
        }
        inode->directBlocks[fileBlockNumber] = (uint16_t) physicalBlock;
        needBlock = true;
    }else{
        physicalBlock = inode->directBlocks[fileBlockNumber];
    }

    size_t bytesToWriteFromLocation = (size_t)512 - byteAtPositionInFileBlock;
    if(bytesToWriteFromLocation > nbytes){
        bytesToWriteFromLocation = nbytes;
    }

    char readDataBlock[512];
    if(needBlock){
        memset(readDataBlock, '\0', 512);
        memcpy(readDataBlock, data, 512);
    }else{
        block_store_read(blockStore, physicalBlock, readDataBlock);
        memcpy((readDataBlock + byteAtPositionInFileBlock), data, bytesToWriteFromLocation);
    }

    block_store_write(blockStore, physicalBlock, readDataBlock);
    nbytes -= bytesToWriteFromLocation;

    if(nbytes > 0 && fileBlockNumber + 1 < 6){
        return bytesToWriteFromLocation + handleDirectBlocks(blockStore, fileBlockNumber + 1 , 0 , inode , (data + bytesToWriteFromLocation) ,nbytes);
    }else if(nbytes > 0 && fileBlockNumber + 1 >= 6){

        uint16_t readIndirectData[256];
        physicalBlock = inode->indirectBlock;
        if (physicalBlock == 0) {
            physicalBlock = block_store_allocate(blockStore);
            //Making sure I don't run out of space.
            if(physicalBlock == SIZE_MAX){
                return 0;
            }
            inode->indirectBlock = (uint16_t) physicalBlock;
            memset(readIndirectData, 0, 512);
            block_store_write(blockStore, physicalBlock, readIndirectData);
        }
        return bytesToWriteFromLocation + handleIndirectBlock(blockStore, fileBlockNumber + 1 , 0 , inode, (data + bytesToWriteFromLocation) ,nbytes, physicalBlock);
    }else{
        return bytesToWriteFromLocation;
    }

}

//Normal Indirect case
ssize_t readIndirectBlock(block_store_t* blockStore, int fileBlockNumber, int byteAtPositionInFileBlock, inode_t* inode,  char* data, size_t nbytes, size_t blockStoreBlockId){
    int indirectBlockLocation = (fileBlockNumber - 6) % 256;
    ssize_t totalBytesRead = 0;
    uint16_t readIndirectData[256];
    if(blockStoreBlockId == 0){
        return 0;
    }
    block_store_read(blockStore, blockStoreBlockId, readIndirectData);
    while(nbytes > 0 && indirectBlockLocation < 256) {

        char tempBlockdata[512];
        if(readIndirectData[indirectBlockLocation] == 0){
            return totalBytesRead;
        }
        block_store_read(blockStore, readIndirectData[indirectBlockLocation], tempBlockdata);
        size_t bytesToReadFromLocation = (size_t)512 - byteAtPositionInFileBlock;
        //Checking for writing an excess amount.
        if(bytesToReadFromLocation > nbytes){
            bytesToReadFromLocation = nbytes;
        }
        memcpy(data,tempBlockdata+byteAtPositionInFileBlock ,bytesToReadFromLocation);

        fileBlockNumber++;
        nbytes -= bytesToReadFromLocation;
        indirectBlockLocation++;
        data += bytesToReadFromLocation;
        totalBytesRead += bytesToReadFromLocation;
        byteAtPositionInFileBlock = 0;
    }

    if(nbytes > 0){
        return totalBytesRead + readDoubleIndirectBlocks(blockStore, fileBlockNumber, 0, inode, data, nbytes);
    }

    return totalBytesRead;

}
//Normal Indirect case
ssize_t handleIndirectBlock(block_store_t* blockStore, int fileBlockNumber, int byteAtPositionInFileBlock, inode_t* inode, const char* data, size_t nbytes, size_t blockStoreBlockId){

    int indirectBlockLocation = (fileBlockNumber - 6) % 256;
    ssize_t totalBytesWritten = 0;
    size_t physicalBlock = 0;
    uint16_t readIndirectData[256];
    //Have indirect data
    block_store_read(blockStore, blockStoreBlockId, readIndirectData);
    while(nbytes > 0 && indirectBlockLocation < 256) {

        if(readIndirectData[indirectBlockLocation] == 0){
            //Allocating space for the block.
            physicalBlock = block_store_allocate(blockStore);
            if(physicalBlock == SIZE_MAX){
                block_store_write(blockStore, blockStoreBlockId, readIndirectData);
                return totalBytesWritten;
            }
            //Setting it the block that was allocated.
            readIndirectData[indirectBlockLocation] = physicalBlock;
        }
        //Can starting writing to the block.
        char tempBlockdata[512];
        block_store_read(blockStore, readIndirectData[indirectBlockLocation], tempBlockdata);
        size_t bytesToWriteFromLocation = (size_t)512 - byteAtPositionInFileBlock;
        //Checking for writing an excess amount.
        if(bytesToWriteFromLocation > nbytes){
            bytesToWriteFromLocation = nbytes;
        }
        memcpy(tempBlockdata+byteAtPositionInFileBlock, data, bytesToWriteFromLocation);
        block_store_write(blockStore, readIndirectData[indirectBlockLocation], tempBlockdata);

        fileBlockNumber++;
        nbytes -= bytesToWriteFromLocation;
        indirectBlockLocation++;
        data += bytesToWriteFromLocation;
        totalBytesWritten += bytesToWriteFromLocation;
        byteAtPositionInFileBlock = 0;
    }

    block_store_write(blockStore, blockStoreBlockId, readIndirectData);

    if(nbytes > 0){
        return totalBytesWritten + handleDoubleIndirectBlocks(blockStore, fileBlockNumber, 0, inode, data, nbytes);
    }

    return totalBytesWritten;

}

//For double Indirect
ssize_t readDoubleIndirectBlocks(block_store_t* blockStore, int fileBlockNumber, int byteAtPositionInFileBlock, inode_t* inode, char* data, size_t nbytes){
    uint16_t readDoubleIndirectData[256];
    if(inode->doubleIndirectBlock == 0){
        return 0;
    }
    block_store_read(blockStore, inode->doubleIndirectBlock, readDoubleIndirectData);
    int index = (fileBlockNumber - 261) / 256;
    if(readDoubleIndirectData[index] == 0){
        return 0;
    }
    return readIndirectBlock(blockStore, fileBlockNumber, byteAtPositionInFileBlock, inode, data, nbytes, readDoubleIndirectData[index]);

}
//For double Indirect
ssize_t handleDoubleIndirectBlocks(block_store_t* blockStore, int fileBlockNumber, int byteAtPositionInFileBlock, inode_t* inode, const char* data, size_t nbytes){

    size_t physicalBlock = 0;
    uint16_t readDoubleIndirectData[256];

    if(inode->doubleIndirectBlock == 0){

        physicalBlock = block_store_allocate(blockStore);
        if(physicalBlock == SIZE_MAX){
            return 0;
        }
        inode->doubleIndirectBlock = physicalBlock;
        //Clearing out the array.
        memset(readDoubleIndirectData, 0, 512);

    } else {
        block_store_read(blockStore, inode->doubleIndirectBlock, readDoubleIndirectData);
    }
    //Where in the doubleIndirect block.
    int index = (fileBlockNumber - 261) / 256;
    if(readDoubleIndirectData[index] == 0){
        physicalBlock = block_store_allocate(blockStore);
        if(physicalBlock == SIZE_MAX){
            return 0;
        }
        readDoubleIndirectData[index] = physicalBlock;
        block_store_write(blockStore, inode->doubleIndirectBlock, readDoubleIndirectData);
        return handleIndirectBlock(blockStore, fileBlockNumber, byteAtPositionInFileBlock, inode, data, nbytes, physicalBlock);
    } else{
        return handleIndirectBlock(blockStore, fileBlockNumber, byteAtPositionInFileBlock, inode, data, nbytes, readDoubleIndirectData[index]);
    }
}


///
/// Deletes the specified file and closes all open descriptors to the file
///   Directories can only be removed when empty
/// \param fs The F17FS containing the file
/// \param path Absolute path to file to remove
/// \return 0 on success, < 0 on error
///
int fs_remove(F17FS_t *fs, const char *path){

    if(fs == NULL || path == NULL || strcmp(path, "") == 0){
        return -1;
    }

    inode_t* inodeForParent = calloc(1, sizeof(inode_t));
    directory_t* parentDirectory = calloc(1, sizeof(directory_t));
    file_record_t* file = calloc(1, sizeof(file_record_t));

    int succesfullyTraversed = traverseFilePath(path, fs, parentDirectory, inodeForParent, file);
    if(succesfullyTraversed < 0){
        free(file);
        free(parentDirectory);
        free(inodeForParent);
        return -1;
    }

    int fileLocation = indexOfNameInDirectoryEntries(*parentDirectory, file->name);
    if(fileLocation < 0){
        free(inodeForParent);
        free(parentDirectory);
        free(file);
        return -1;
    }
    //Check to see if its directory.
    if(parentDirectory->entries[fileLocation].type == FS_DIRECTORY){
        inode_t* temp = calloc(1, sizeof(inode_t));
        directory_t* checkingDirectory = calloc(1, sizeof(directory_t));
        getInodeFromTable(fs, parentDirectory->entries[fileLocation].inodeNumber, temp);
        block_store_read(fs->blockStore, temp->directBlocks[0], checkingDirectory);
        int i = 0;
        for(i = 0; i<7; i++){
            if(checkingDirectory->entries[i].inodeNumber != '\0'){
                free(inodeForParent);
                free(parentDirectory);
                free(file);
                free(temp);
                free(checkingDirectory);
                return -1;
            }
        }

        uint8_t copyOfInodeToUpdate = parentDirectory->entries[fileLocation].inodeNumber;
        //Clears out the directory.
        memset(checkingDirectory,0, sizeof(directory_t));
        block_store_write(fs->blockStore, temp->directBlocks[0], checkingDirectory);
        block_store_release(fs->blockStore, temp->directBlocks[0]);
        free(checkingDirectory);
        //Clears out the inode
        memset(temp, 0, sizeof(inode_t));
        writeInodeIntoTable(fs,copyOfInodeToUpdate, temp);
        free(temp);

        //Reseting the parent directory.
        parentDirectory->entries[fileLocation].inodeNumber = '\0';
        memset(parentDirectory->entries[fileLocation].name, '\0', 64);
        block_store_write(fs->blockStore, inodeForParent->directBlocks[0], parentDirectory);

        superRoot_t* root = calloc(1, sizeof(superRoot_t));
        block_store_read(fs->blockStore,0,root);
        root->bitmap = bitmap_overlay(256, root->freeInodeMap);
        bitmap_reset(root->bitmap, copyOfInodeToUpdate);
        bitmap_destroy(root->bitmap);
        block_store_write(fs->blockStore,0, root);
        free(root);
    } else {
        inode_t* temp = calloc(1, sizeof(inode_t));
        getInodeFromTable(fs, parentDirectory->entries[fileLocation].inodeNumber, temp);

        int i = 0;
        //Dealing with direct blocks.
        for(i = 0; i<7; i++){
            if(temp->directBlocks[i] != 0){
                block_store_release(fs->blockStore, temp->directBlocks[i]);
            }
        }
        //Deals with indirect block.
        if(temp->indirectBlock != 0){
            uint16_t indirectData[256];
            block_store_read(fs->blockStore, temp->indirectBlock, indirectData);
            for(i = 0; i<256; i++){
                if(indirectData[i] != 0){
                    block_store_release(fs->blockStore, indirectData[i]);
                }
            }
            block_store_release(fs->blockStore, temp->indirectBlock);
        }

        //Deals with double Indirect block.
        if(temp->doubleIndirectBlock != 0){

            uint16_t doubleIndirectData[256];
            block_store_read(fs->blockStore, temp->doubleIndirectBlock, doubleIndirectData);

            for(i = 0; i<256; i++){

                if(doubleIndirectData[i] != 0){

                    uint16_t indirectData[256];
                    block_store_read(fs->blockStore, doubleIndirectData[i], indirectData);
                    int j = 0;
                    for(j = 0; i<256; i++){

                        if(indirectData[j] != 0){
                            block_store_release(fs->blockStore, indirectData[j]);
                        }
                    }
                    block_store_release(fs->blockStore, doubleIndirectData[i]);
                }
            }
            block_store_release(fs->blockStore, temp->doubleIndirectBlock);
        }
        uint8_t copyOfInodeToUpdate = parentDirectory->entries[fileLocation].inodeNumber;

        memset(temp, 0, sizeof(inode_t));
        writeInodeIntoTable(fs,copyOfInodeToUpdate, temp);
        free(temp);

        //Reseting the parent directory.
        parentDirectory->entries[fileLocation].inodeNumber = '\0';
        memset(parentDirectory->entries[fileLocation].name, '\0', 64);
        block_store_write(fs->blockStore, inodeForParent->directBlocks[0], parentDirectory);

        superRoot_t* root = calloc(1, sizeof(superRoot_t));
        block_store_read(fs->blockStore,0,root);
        root->bitmap = bitmap_overlay(256, root->freeInodeMap);
        bitmap_reset(root->bitmap, copyOfInodeToUpdate);
        bitmap_destroy(root->bitmap);
        block_store_write(fs->blockStore,0, root);
        free(root);
    }

    free(inodeForParent);
    free(parentDirectory);
    free(file);

    return 0;
}

/// Moves the file from one location to the other
///   Moving files does not affect open descriptors
/// \param fs The F17FS containing the file
/// \param src Absolute path of the file to move
/// \param dst Absolute path to move the file to
/// \return 0 on success, < 0 on error
///
int fs_move(F17FS_t *fs, const char *src, const char *dst){
    if(fs == NULL || src == NULL || dst == NULL){
        return -1;
    }
    return 0;
}

//HELPER FUNCTIONS!!!
int traverseFilePath(const char *path, F17FS_t *fs, directory_t* parentDirectory ,inode_t* inode, file_record_t* file){

    if(path[0] != '/') {
        return -1;
    }

    if(strlen(path) == 1){
        return -1;
    }
    size_t i;
    //Used to keep track of current location in string.
    int currentIndexOfFileName = 0;
    //Need to Read blocksize of Inodes to get the first inode off.
    inode_t* blockSizeOfInodes = calloc(8, sizeof(inode_t));
    block_store_read(fs->blockStore, 1 , blockSizeOfInodes);
    //Getting the first Inode.
    memcpy(inode,&blockSizeOfInodes[0], sizeof(inode_t));
    //inode = &blockSizeOfInodes[0];
    //Getting the root directory from the dataBlocks of the first inode.
    block_store_read(fs->blockStore, inode->directBlocks[0], parentDirectory);

    for (i = 1; i < strlen(path); i++) {

        if (path[i] == '/') {
            //Appending null terminator.
            file->name[currentIndexOfFileName] = '\0';
                //Check if fileName exists already in blockStore.
            int indexOfExistingDirectory = indexOfNameInDirectoryEntries(*parentDirectory, file->name);
            if(indexOfExistingDirectory >= 0){

                //Getting inode from directory.
                getInodeFromDirectory(fs, parentDirectory, indexOfExistingDirectory, inode);
                //Checking if parentDirectory is a file.
                if(inode->fileMode < 1000){
                    free(blockSizeOfInodes);
                    return -1;
                }
                //Updating the parentDirectory with new inode
                block_store_read(fs->blockStore, inode->directBlocks[0], parentDirectory);
                //Resetting the string.
                currentIndexOfFileName = 0;
                memset(file->name, '\0',64);
            }else{
                //Checking if fileName is a directory.
                free(blockSizeOfInodes);
                return -1;
            }

        } else if(currentIndexOfFileName >= 63) {
            //Checking if fileName is too long.
            free(blockSizeOfInodes);
            return -1;
        } else {
            file->name[currentIndexOfFileName] = path[i];
            currentIndexOfFileName++;
        }
    }
    free(blockSizeOfInodes);
    return 0;
}

int checkBlockInDirectory(directory_t* directory, file_record_t* file) {
    int i;
    for (i = 0; i < 7; i++) {
        if (directory->entries[i].inodeNumber == '\0') {
            return i;
        }
        if (strcmp(directory->entries[i].name, file->name) == 0)
        {
            return -1;
        }
    }
    return -1;
}

void getInodeFromDirectory(F17FS_t* fs,directory_t* parentDirectory, int index, inode_t* inode){
    getInodeFromTable(fs, parentDirectory->entries[index].inodeNumber, inode);
}

void getInodeFromTable(F17FS_t* fs, int index, inode_t* inode) {
    inode_t* blockSizeOfInodes = calloc(8, sizeof(inode_t));
    size_t inodeBlocks = (size_t) (index/8)+1;
    size_t inodeId = (size_t) (index) % 8;
    block_store_read(fs->blockStore, inodeBlocks, blockSizeOfInodes);
    memcpy(inode, &blockSizeOfInodes[inodeId], sizeof(inode_t));
    free(blockSizeOfInodes);
}

void writeInodeIntoTable(F17FS_t* fs, size_t index, inode_t* inode) {
    inode_t* blockSizeOfInodes = calloc(8, sizeof(inode_t));
    size_t inodeBlocks = (size_t) (index/8)+1;
    size_t inodeId = (size_t) (index) % 8;
    block_store_read(fs->blockStore, inodeBlocks, blockSizeOfInodes);
    memcpy(&blockSizeOfInodes[inodeId], inode, sizeof(inode_t));
    block_store_write(fs->blockStore, inodeBlocks, blockSizeOfInodes);
    free(blockSizeOfInodes);
}

int indexOfNameInDirectoryEntries(directory_t directory, char* fileName){
    int i;
    for(i = 0; i<7; i++)
    {
        if(strcmp(directory.entries[i].name, fileName) == 0) {
            return i;
        }
    }
    return -1;
}

off_t calculateOffset(int fileSize, off_t seekLocation){
    if(seekLocation <= 0){
        return 0;
    }else if(seekLocation > fileSize){
        return fileSize;
    }else{
        return seekLocation;
    }
}
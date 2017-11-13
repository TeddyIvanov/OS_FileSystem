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
    if(fs == NULL || fd < 0 || offset <= 0 || whence == FS_SEEK_END){
        return -1;
    }
    //Check the type of offSet Enum FS_SEEK_SET, FS_SEEK_CUR, FS_SEEK_END
    //If FS_SEEK_END, jump to the end and subtrack from it.
    //IF FS_SEEK_CUR, add or subtract from location.
    //If FS_SEEK_SET, add from beginning file.
    //Update the file descriptor and update the bitmap.
    //Clean up.
    return -1;
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

    if(fs == NULL || fd < 0 || dst == NULL ||  nbyte <= 0){
        return -1;
    }
    //Check if file exists so that it can write too it.
    //Check to see its not a directory
    //Open up fileDescriptor since it are reading to it.
    //If buffer greater than the EOF, error out, reading past buffer.
    //Grab the corresponding datablocks to read, check if reading too much.
    //Clear out the descriptor after reading from file.
    //Clean up after.
    return -1;
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
    if(fs == NULL || fd < 0 || src == NULL || nbyte <= 0){
        return -1;
    }
    //Check if file exists so that it can write too it.
    //Check to see its not a directory
    //Open up fileDescriptor since it are writing to it.
    //If buffer greater than the EOF then extend the file,
    //Check to see if we are not overflowing out of Blockstore.
    //Get more datablocks for writing and update the blockstore.
    //Update the bitmap and blockstore that we have made changes to the datablocks.
    //Clear out the descriptor after writing to the file.
    //Clean up after.
    return -1;
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
    //Traverse down to see if the file exists,
    //If its a directory and there are no file, then remvoe directory.
    //Get the inodeNumber that corresponds to the specific file.
    //Check if any fileDescriptors exist with correspodining file.
    //Check all file descriptors with that inodeNumber.
    //Set them back.
    //Clear out the correspoinding datablock.
    //Update the bitmap with file descriptors.
    //Store back into the blockStore.

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
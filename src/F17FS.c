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

struct dir_files{ //65 Bytes total
    char fileName[64]; //64 bytes
    uint8_t inodeNumber; //1 bytes
};

struct directory{ //455 Bytes total
    dir_files_t entries[7]; //7*65 bytes
};

struct superRoot{
    //For Inodes
    bitmap_t* bitmap;
    size_t freeBlocks;
    size_t totalBlocks;
    size_t blockSize;
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
    root->bitmap = bitmap_create(BLOCK_SIZE_BYTES/2);
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

    //Writes the blockStore to disc
    block_store_serialize(blockStore, path);

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
/*
int fs_create(F17FS_t *fs, const char *path, file_t type) {

    //Error check file path for Null.
//  if(path == NULL || strcmp(path, "") == 0 || type == NULL){
//      return -1;
//  }
    fs = NULL;
    path = NULL;
    type = 0;
    return 0;
}

/// Opens the specified file for use
///   R/W position is set to the beginning of the file (BOF)
///   Directories cannot be opened
/// \param fs The F17FS containing the file
/// \param path path to the requested file
/// \return file descriptor to the requested file, < 0 on error
int fs_open(F17FS_t *fs, const char *path) {
    fs = NULL;
    path = NULL;
    return 0;
}

/// Closes the given file descriptor
/// \param fs The F17FS containing the file
/// \param fd The file to close
/// \return 0 on success, < 0 on failure
int fs_close(F17FS_t *fs, int fd){
    fs = NULL;
    fd = 0;
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
    fs = NULL;
    path = NULL;
    return NULL;
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
    fs = NULL;
    fd = 0;
    offset = 0;
    whence = 1;
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
    fs = NULL;
    fd = 0;
    dst = NULL;
    nbyte = 0;
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
    fs = NULL;
    fd = 0;
    src = NULL;
    nbyte = 0;
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
    fs = NULL;
    path = NULL;
    return -1;
}

/// Moves the file from one location to the other
///   Moving files does not affect open descriptors
/// \param fs The F17FS containing the file
/// \param src Absolute path of the file to move
/// \param dst Absolute path to move the file to
/// \return 0 on success, < 0 on error
///
int fs_move(F17FS_t *fs, const char *src, const char *dst){
    fs = NULL;
    src = NULL;
    dst = NULL;
    return -1;
}
*/
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "block_store.h"
#include "bitmap.h"


#define BLOCK_STORE_NUM_BLOCKS 65536   // 2^16 blocks.
#define BLOCK_STORE_AVAIL_BLOCKS 65520 // Last 16 blocks consumed by the FBM
#define BLOCK_SIZE_BITS 4096         // 2^9 BYTES per block *2^3 BITS per BYTES
#define BLOCK_SIZE_BYTES 512         // 2^9 BYTES per block
#define BLOCK_STORE_NUM_BYTES (BLOCK_STORE_NUM_BLOCKS * BLOCK_SIZE_BYTES)  // 2^16 blocks of 2^9 bytes.



struct block_store {
    int fd;
    uint8_t *data_blocks;
    bitmap_t *fbm;
};

int create_file(const char *const fname) {
    if (fname) {
        int fd = open(fname, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (fd != -1) {
            if (ftruncate(fd, BLOCK_STORE_NUM_BYTES) != -1) {
                return fd;
            }
            close(fd);
        }
    }
    return -1;
}
int check_file(const char *const fname) {
    if (fname) {
        int fd = open(fname, O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (fd != -1) {
            struct stat file_info;
			if (fstat(fd, &file_info) != -1 && file_info.st_size >= BLOCK_STORE_NUM_BYTES && file_info.st_size <= BLOCK_STORE_NUM_BYTES + BLOCK_STORE_NUM_BYTES/8 ) {
            //if (fstat(fd, &file_info) != -1 && file_info.st_size == BLOCK_STORE_NUM_BYTES) {
                return fd;
            }
            close(fd);
        }
    }
    return -1;
}

block_store_t *block_store_init(const bool init, const char *const fname) {
    if (fname) {
        block_store_t *bs = (block_store_t *) malloc(sizeof(block_store_t));
        if (bs) {
            bs->fd = init ? create_file(fname) : check_file(fname);
            if (bs->fd != -1) {
                bs->data_blocks = (uint8_t *) mmap(NULL, BLOCK_STORE_NUM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, bs->fd, 0);
                if (bs->data_blocks != (uint8_t *) MAP_FAILED) {
                         if (init) {
                                memset(bs->data_blocks, 0X00, BLOCK_STORE_NUM_BYTES);
								bs->data_blocks[BLOCK_STORE_NUM_BYTES - 1] = 0xff;
								bs->data_blocks[BLOCK_STORE_NUM_BYTES - 2] = 0xff;
                          }
                          bs->fbm = bitmap_overlay(BLOCK_STORE_NUM_BLOCKS, bs->data_blocks + BLOCK_STORE_AVAIL_BLOCKS*BLOCK_SIZE_BYTES);
                          if (bs->fbm) {
                                return bs;
                           }
                           munmap(bs->data_blocks, BLOCK_STORE_NUM_BYTES);
                }
                close(bs->fd);
            }
            free(bs);
        }
    }
    return NULL;
}


///
///-- Create a new BS device.
///-- Return pointer to the new block storage device, NULL on error
///
block_store_t *block_store_create(const char *const fname) {
    return block_store_init(true, fname);
    }
//
block_store_t *block_store_open(const char *const fname) {
    return block_store_init(false, fname);
}


///
///-- Destroy the provided block storage device
///-- \param bs BS device
///
void block_store_destroy(block_store_t *const bs) {
      if (bs) {
        bitmap_destroy(bs->fbm);
        munmap(bs->data_blocks, BLOCK_STORE_NUM_BYTES);
        close(bs->fd);
        free(bs);
    }
}


///
///-- Search for a free block, marks it as in use, and return the block's id
/// \param bs BS device
/// \return Allocated block's id, SIZE_MAX on error
///
size_t block_store_allocate(block_store_t *const bs) {
    if (bs == NULL) {
        return SIZE_MAX; // return SIZE_MAX if the input is a null pointer
    }
    //-- find first zero in the bitmap
    size_t id;
    id = bitmap_ffz(bs->fbm); // index of the first free block
    if (id == SIZE_MAX) {
        return SIZE_MAX; // return SIZE_MAX since the last block is not available for storing data
    }
    bitmap_set(bs->fbm, id); // mark it as in use
  //  bitmap_destroy(bs->fbm); // destruct and destroy bitmap object
    return id;
}

///
///-- Attempts to allocate the requested block id
/// \param bs the block store object
/// \block_id the requested block identifier
/// \return boolean indicating succes of operation
///
bool block_store_request(block_store_t *const bs, const size_t block_id) {
    if (block_id > BLOCK_STORE_AVAIL_BLOCKS || bs == NULL) {
        return false;
    }
    bool blockUsed = 0;
    blockUsed = bitmap_test(bs->fbm, block_id); // check if the block is in use
    if (blockUsed) { // if this block is already in use
        //bitmap_destroy(bs->fbm); // destruct and destroy bitmap object
        return false;
    }
    else { // if this block is not in use
        bitmap_set(bs->fbm, block_id); // mark the block as in use
        //bitmap_destroy(bs->fbm); // destruct and destroy bitmap object
        return true;
    }
}

///
///-- Frees the specified block
/// \param bs BS device
/// \param block_id The block to free
///
void block_store_release(block_store_t *const bs, const size_t block_id) {
    if (block_id <= BLOCK_STORE_AVAIL_BLOCKS && bs != NULL) {
        bool success = 0;
        success = bitmap_test(bs->fbm, block_id); // check if the block is in use
        if (success) {
            bitmap_reset(bs->fbm, block_id); // clear requested bit in bitmap
    //        bitmap_destroy(bs->fbm); // destruct and destroy bitmap object
        }
    }
    //// Some error message here ////
}

///
///-- Counts the number of blocks marked as in use
/// \param bs BS device
/// \return Total blocks in use, SIZE_MAX on error
///
size_t block_store_get_used_blocks(const block_store_t *const bs) {
    if (bs) {
        size_t numSet = 0;
        numSet = bitmap_total_set(bs->fbm); // count all bits set
      //  bitmap_destroy(bs->fbm); // destruct and destroy bitmap object
        return numSet;
    }
    return SIZE_MAX;
}

///
///-- Counts the number of blocks marked free for use
/// \param bs BS device
/// \return Total blocks free, SIZE_MAX on error
///
size_t block_store_get_free_blocks(const block_store_t *const bs) {
    if (bs) {
        size_t numSet = 0;
        size_t numZero = 0;
        numSet = bitmap_total_set(bs->fbm); // count all bits set
        //bitmap_destroy(bs->fbm); // destruct and destroy bitmap object
        numZero = BLOCK_STORE_AVAIL_BLOCKS - numSet; // count zero bits
        return numZero;
    }
    return SIZE_MAX;
}

///
///-- Returns the total number of user-addressable blocks
///  (since this is constant, you don't even need the bs object)
/// \return Total blocks
///
size_t block_store_get_total_blocks() {
    return BLOCK_STORE_AVAIL_BLOCKS;
}

///
///-- Reads data from the specified block and writes it to the designated buffer
/// \param bs BS device
/// \param block_id Source block id
/// \param buffer Data buffer to write to
/// \return Number of bytes read, 0 on error
///
size_t block_store_read(const block_store_t *const bs, const size_t block_id, void *buffer) {
    if (bs && buffer && block_id <= BLOCK_STORE_AVAIL_BLOCKS) {
        memcpy(buffer, bs->data_blocks+block_id*BLOCK_SIZE_BYTES, BLOCK_SIZE_BYTES);
        return BLOCK_SIZE_BYTES;
    }
    return 0;
}

///
///-- Reads data from the specified buffer and writes it to the designated block
/// \param bs BS device
/// \param block_id Destination block id
/// \param buffer Data buffer to read from
/// \return Number of bytes written, 0 on error
///
size_t block_store_write(block_store_t *const bs, const size_t block_id, const void *buffer) {
    if (bs && buffer && block_id <= BLOCK_STORE_AVAIL_BLOCKS) {
        memcpy(bs->data_blocks+block_id*BLOCK_SIZE_BYTES, buffer, BLOCK_SIZE_BYTES);
        return BLOCK_SIZE_BYTES;
    }
    return 0;
}

///
///-- Imports BS device from the given file - for grads/bonus
/// \param filename The file to load
/// \return Pointer to new BS device, NULL on error
///
block_store_t *block_store_deserialize(const char *const filename) {
    if (filename) {
        int fd = open(filename, O_RDONLY); // open file (read only)
        if (fd < 0) { // if opening file fails
            return 0;
        }
        block_store_t *bs = NULL;
        bs = block_store_create(filename);
        int df_read1, df_read2;
        df_read1 = read(fd, bs->data_blocks, BLOCK_STORE_AVAIL_BLOCKS*BLOCK_SIZE_BYTES); // read bs->Data from the file
        df_read2 = read(fd, bs->fbm, BLOCK_STORE_NUM_BLOCKS/8); // read bs->FBM from the file
        if (df_read1 < 0 || df_read2 < 0) { // if the system call returns an error
            return 0;
        }
        return bs;
    }
    return 0;
}

///
///-- Writes the entirety of the BS device to file, overwriting it if it exists - for grads/bonus
/// \param bs BS device
/// \param filename The file to write to
/// \return Number of bytes written, 0 on error
///
size_t block_store_serialize(const block_store_t *const bs, const char *const filename) {
    if (bs && filename) {
        int fd = open(filename, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR); // open file (write only)
        if (fd < 0) { // if opening file fails
            return 0;
        }
        write(fd, bs->data_blocks, BLOCK_STORE_AVAIL_BLOCKS*BLOCK_SIZE_BYTES); // write bs->Data to file
        write(fd, bs->fbm, BLOCK_STORE_NUM_BLOCKS/8); // write bs->FBM to file
        close(fd); // close file
        size_t wr_size = block_store_get_used_blocks(bs); // number of block in use
        return (wr_size*BLOCK_SIZE_BYTES); // return number of bytes written
    }
    return 0;
}








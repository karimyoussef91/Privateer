#pragma once

#define _GNU_SOURCE
#include <inttypes.h>
#include <sys/types.h>
#include <stdio.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>

#include <assert.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <iostream>
#include <list>
#include <set>
#include <queue>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <thread>
#include <chrono>
// #include <mutex>
// #include <omp.h>

#include "block_storage.hpp"
#include "utility/fault_event.hpp"
#include "utility/event_queue.hpp"

#ifdef USE_COMPRESSION
#include "utility/compression.hpp"
#endif

class virtual_memory_manager {
  public:
    virtual_memory_manager(void* start_address, size_t region_max_capacity, 
                                                    std::string versioin_metadata_path, std::string blocks_path, std::string stash_path, bool allow_overwrite);
    virtual_memory_manager(void* addr, std::string version_metadata_path, std::string stash_path, bool read_only);
    ~virtual_memory_manager();
    void msync();
    void* handler();
    static void* handler_helper(void* context);
    void set_uffd(uint64_t uffd);
    void* get_region_start_address();
    size_t static version_capacity(std::string version_path);
    size_t current_region_capacity();
    bool snapshot(const char* version_metadata_path);
    int close();
    void deactivate_uffd_thread();
    void add_page_fault_event(utility::fault_event fevent);
    void add_page_fault_event_all(utility::fault_event fevent);
    size_t get_block_size();
    uint64_t get_block_address(uint64_t fault_address);
  private:
    void* m_region_start_address;
    size_t m_block_size;
    size_t m_region_max_capacity;
    size_t m_max_mem_size;
    std::string m_version_metadata_path;
    bool m_read_only;
    int metadata_fd;
    int prot;
    std::vector<std::list<uint64_t>> clean_lru; // Change to sub-regions (declaration [done], usage [done])
    std::vector<std::list<uint64_t>> dirty_lru; // Change to sub-regions (declaration [done], usage [done])
    std::vector<std::set<uint64_t>> stash_set; // Change to sub-regions (declaration [done], usage [done])
    std::vector<std::set<uint64_t>> present_blocks; // Change to sub-regions (declaration [done], usage [done])
    std::vector<std::mutex> sub_regions_mutex_list;
    std::string * blocks_ids; 
    
    static const size_t FILE_GRANULARITY_DEFAULT_BYTES;
    static const size_t MAX_MEM_DEFAULT_BLOCKS;
    static const size_t HASH_SIZE;
    static const std::string EMPTY_BLOCK_HASH;

    block_storage *m_block_storage;

    std::mutex* blocks_locks;
    std::mutex add_event_mutex;
    pthread_mutex_t handler_mutex;
    static std::mutex handler_mutex_global;
    long m_uffd;
    std::atomic<bool> uffd_active;
    int uffd_pipe[2];
    // size_t num_locks = 2048;

    utility::event_queue<utility::fault_event> *fault_events_queue;
    const int num_handling_threads = 8; // 1;
    std::vector<pthread_t> fault_handling_threads; // Change to sub-regions (declaration [done], usage (TODO))
    std::atomic<long> debug = 0;

    void evict_if_needed(int sub_region_index);
    void update_metadata(int sub_region_index);
    void create_version_metadata(const char* version_metadata_dir_path, const char* block_storage_dir_path, size_t version_capacity, bool allow_overwrite);
    void start_handler_thread();
    void stop_handler_thread();
    void msync(int sub_region_index);
};

const size_t virtual_memory_manager::FILE_GRANULARITY_DEFAULT_BYTES = 134217728; // 128 MBs 
const size_t virtual_memory_manager::MAX_MEM_DEFAULT_BLOCKS = 65536;
const size_t virtual_memory_manager::HASH_SIZE = 64;
const std::string virtual_memory_manager::EMPTY_BLOCK_HASH = "0000000000000000000000000000000000000000000000000000000000000000";
std::mutex virtual_memory_manager::handler_mutex_global;
// utility::event_queue<utility::fault_event> fault_events_queue(1);
// TODO IMPORTANT: Make create() and open() interfaces
// Create
virtual_memory_manager::virtual_memory_manager(void* start_address,size_t region_max_capacity,
                                                    std::string version_metadata_path, std::string blocks_path, std::string stash_path, bool allow_overwrite){
  // printf("Waiting on virtual_memory_manager::handler_mutex_global create Thread ID: %ld\n", (uint64_t) syscall(SYS_gettid));
  const std::lock_guard<std::mutex> lock(virtual_memory_manager::handler_mutex_global);
  // printf("Aquired virtual_memory_manager::handler_mutex_global create Thread ID: %ld\n", (uint64_t) syscall(SYS_gettid));
  // std::cout << "VMM CREATING\n";
  // Verify system page alignment
  size_t pagesize = sysconf(_SC_PAGE_SIZE);
  if ( ((uint64_t) start_address) % pagesize != 0){
    std::cerr << "Error: start_address is not system-page aligned" << std::endl;
    exit(-1);
  }
  
  // Set block_size
  m_block_size = utility::get_environment_variable("PRIVATEER_BLOCK_SIZE");
  if ( std::isnan(m_block_size) || m_block_size == 0){
    size_t num_blocks = utility::get_environment_variable("PRIVATEER_NUM_BLOCKS");
    if (std::isnan(num_blocks) || num_blocks == 0){
      // std::cout << "Setting Privateer block size to default of : " << FILE_GRANULARITY_DEFAULT_BYTES << " bytes." << std::endl;
      m_block_size = FILE_GRANULARITY_DEFAULT_BYTES;
    }
    else{
      if (region_max_capacity % num_blocks == 0){
        m_block_size = region_max_capacity / num_blocks;
      }
      else{
        std::cerr << "PRIVATEER_NUM_BLOCKS is set, but region capacity is not divisible by it "<< std::endl;
        exit(-1);
      }
    }
  }
  // Verify multiple of system's page size
  if (m_block_size % pagesize != 0){
    std::cerr << "Error: block_size must be multiple of system page size (" << pagesize << ")" << std::endl;
    exit(-1);
  }
  if (region_max_capacity < m_block_size){
    std::cout << "WARNING: region capacity less than block size, setting block size to region capacity" << std::endl;
    m_block_size = region_max_capacity;
  }
  // std::cout << "m_block_size after check: " << m_block_size << std::endl;
  size_t max_mem_size_blocks = utility::get_environment_variable("PRIVATEER_MAX_MEM_BLOCKS");
  if ( std::isnan(max_mem_size_blocks) || max_mem_size_blocks == 0){
    max_mem_size_blocks = MAX_MEM_DEFAULT_BLOCKS;
  }
  
  create_version_metadata(version_metadata_path.c_str(), blocks_path.c_str(), region_max_capacity, allow_overwrite);
  
  // m_block_size = block_size;
  m_region_max_capacity = region_max_capacity;
  m_max_mem_size = max_mem_size_blocks * m_block_size;
  // std::cout << "m_max_mem_size = " << m_max_mem_size << std::endl;
  m_version_metadata_path = version_metadata_path;

  m_block_storage = new block_storage(blocks_path, stash_path, m_block_size);

  
  // mmap region with full size
  int flags = MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE;
  if (start_address != nullptr)
  {
    flags |= MAP_FIXED;
  }
  prot = PROT_READ | PROT_WRITE;
  m_region_start_address = mmap(start_address, m_region_max_capacity, prot, flags, -1, 0);
  if (m_region_start_address == MAP_FAILED){
    std::cerr << "virtual_memory_manager: Error mmap-ing region starting address -  " << strerror(errno)<< std::endl;
    exit(-1);
  }

  size_t num_blocks = m_region_max_capacity / m_block_size;
  // std::cout << "num_blocks: " << num_blocks << std::endl;
  blocks_ids = new std::string[num_blocks];
  // blocks_locks = new std::mutex[num_blocks];
  // std::cout << "DEBUG: before init blocks_ids" << std::endl;
  for (size_t i = 0 ; i < num_blocks ; i++){
    blocks_ids[i] = EMPTY_BLOCK_HASH;
  }
  // std::cout << "DEBUG: after init blocks_ids" << std::endl;
  
  /* struct stat st_dev_null;
  if (fstat(0,&st_dev_null) != 0){
    // std::cout << "Opening /dev/null" << std::endl;
    int dev_null_fd = ::open("/dev/null",O_RDWR);
    // std::cout << "/dev/null FD: " << dev_null_fd << std::endl;
  } */

  m_read_only = false;

  uffd_active = true;
  /* pthread_mutex_init(&handler_mutex, NULL);
  
  if (pipe2(uffd_pipe, 0) < 0){
    std::cerr << "Virtual Memory Manager: Error Userfaultfd pipe failed - " << strerror(errno) << std::endl;
    exit(-1);
  } */
  sub_regions_mutex_list = std::vector<std::mutex>(num_handling_threads);
  for (int i = 0; i < num_handling_threads; i++){
    std::list<uint64_t> clean_lru_i;
    std::list<uint64_t> dirty_lru_i; 
    std::set<uint64_t> stash_set_i; 
    std::set<uint64_t> present_blocks_i; 
    std::mutex sub_region_mutex;
    clean_lru.push_back(clean_lru_i);
    dirty_lru.push_back(dirty_lru_i);
    stash_set.push_back(stash_set_i);
    present_blocks.push_back(present_blocks_i);
    // sub_regions_mutex_list.push_back(sub_region_mutex);
  }
  fault_events_queue = new utility::event_queue<utility::fault_event>(num_handling_threads);
  start_handler_thread();
  // printf("Releasing virtual_memory_manager::handler_mutex_global create Thread ID: %ld\n", (uint64_t) syscall(SYS_gettid));
}
// =======================================================================================================

// Open
virtual_memory_manager::virtual_memory_manager(void* addr, std::string version_metadata_path, std::string stash_path, bool read_only){
  // printf("Waiting on virtual_memory_manager::handler_mutex_global open Thread ID: %ld\n", (uint64_t) syscall(SYS_gettid));
  const std::lock_guard<std::mutex> lock(virtual_memory_manager::handler_mutex_global);
  // printf("Aquired virtual_memory_manager::handler_mutex_global open Thread ID: %ld\n", (uint64_t) syscall(SYS_gettid));
  std::cout << "VMM OPENING\n";
  // std::cout << "Opening :)" << std::endl;
  if (read_only){
    // std::cout << "READ ONLY!" << std::endl;
  }
  m_version_metadata_path = version_metadata_path;
  // Read blocks path
  std::string blocks_path_file_name = std::string(m_version_metadata_path) + "/_blocks_path";
  std::ifstream blocks_path_file;
  std::string blocks_dir_path;
  // std::cout << "Privateer Open 218" << std::endl;
  blocks_path_file.open(blocks_path_file_name);
  if (!blocks_path_file.is_open()){
    std::cerr << "Error opening blocks file path at: " << blocks_path_file_name << std::endl;
  }
  if (!std::getline(blocks_path_file, blocks_dir_path)){
    std::cerr << "Error reading blocks path file" << std::endl;
  } 
  // std::cout << "blocks_dir_path = "<< blocks_dir_path << std::endl;
  m_block_storage = new block_storage(blocks_dir_path, stash_path);
  m_block_size = m_block_storage->get_block_granularity();
  // std::cout << "Privateer Open 229" << std::endl;
  std::string metadata_file_name = std::string(m_version_metadata_path) + "/_metadata";
  int flags = read_only? O_RDONLY: O_RDWR;
  int metadata_fd = ::open(metadata_file_name.c_str(), flags, (mode_t) 0666);
  assert(metadata_fd != -1);
  struct stat st;
  fstat(metadata_fd, &st);
  size_t metadata_size = st.st_size;
  // std::cout << "Privateer Open 237" << std::endl;
  // Start: Read capacity file
  m_region_max_capacity = version_capacity(version_metadata_path);
  // std::cout << "Privateer Open 240" << std::endl;
  size_t num_blocks = m_region_max_capacity / m_block_size;
  int mmap_flags = MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE;
  if (addr != nullptr)
  {
    mmap_flags |= MAP_FIXED;
  }
  // std::cout << "Privateer Open 247" << std::endl;
  prot = read_only ? PROT_READ : (PROT_READ | PROT_WRITE);
  
  m_region_start_address = mmap(addr, m_region_max_capacity, prot, mmap_flags, -1, 0);
  if (m_region_start_address == MAP_FAILED){
    std::cerr << "virtual_memory_manager: mmap error - " << strerror(errno)<< std::endl;
    exit(-1);
  }
  // std::cout << "Privateer Open 255" << std::endl;
  // std::cout << "num_blocks: " << num_blocks << std::endl;
  blocks_ids = new std::string[num_blocks];
  char* metadata_content = new char[metadata_size];
  size_t read = ::pread(metadata_fd, (void*) metadata_content, metadata_size, 0);
  if (read == -1){
    std::cerr << "virtual_memory_manager: Error reading metadata - " << strerror(errno) << std::endl;
    exit(-1);
  }
  // std::cout << "Privateer Open 264" << std::endl;
  std::string all_hashes(metadata_content, metadata_size);
  
  uint64_t offset = 0;
  // std::cout << "Privateer: Metadata size = " << metadata_size  << std::endl;
  for (size_t i = 0; i < metadata_size; i += HASH_SIZE){
    // std::cout << "Privateer: Initializing blocks and regions, iteration no. " << i << std::endl;
    // std::cout << "blocks_ids_index: " << (i / HASH_SIZE) << std::endl;
    std::string block_hash(all_hashes, i, HASH_SIZE);
    // std::cout << "before accessing array" << std::endl;
    blocks_ids[i / HASH_SIZE] = block_hash;
  }
  // std::cout << "Privateer Open 275" << std::endl;
  size_t num_occupied_blocks = metadata_size / HASH_SIZE;
  for (size_t i = num_occupied_blocks; i < num_blocks; i++){
    // std::cout << "blocks_ids_index Next: " << i << std::endl;
    blocks_ids[i] = EMPTY_BLOCK_HASH;
  }

  // blocks_locks = new std::mutex[num_blocks];
  
  delete [] metadata_content;
  // std::cout << "Privateer Open 285" << std::endl;

  size_t max_mem_size_blocks = utility::get_environment_variable("PRIVATEER_MAX_MEM_BLOCKS");
  if ( std::isnan(max_mem_size_blocks) || max_mem_size_blocks == 0){
    max_mem_size_blocks = MAX_MEM_DEFAULT_BLOCKS;
  }
  m_max_mem_size = max_mem_size_blocks * m_block_size;
  // std::cout << "Privateer Open 292" << std::endl;
  // In some cases /dev/null file descriptorr was affected, temporary solution is check and re-open
  struct stat st_dev_null;
  if (fstat(0,&st_dev_null) != 0){
    int dev_null_fd = ::open("/dev/null",O_RDWR);
  }
  m_read_only = read_only;
  /* pthread_mutex_init(&handler_mutex, NULL);
  uffd_active = true;
  if (pipe2(uffd_pipe, 0) < 0){
    std::cerr << "Virtual Memory Manager: Error Userfaultfd pipe failed - " << strerror(errno) << std::endl;
    exit(-1);
  } // std::cout << "Privateer Open 304" << std::endl; */
  sub_regions_mutex_list = std::vector<std::mutex>(num_handling_threads);
  for (int i = 0; i < num_handling_threads; i++){
    std::list<uint64_t> clean_lru_i;
    std::list<uint64_t> dirty_lru_i; 
    std::set<uint64_t> stash_set_i; 
    std::set<uint64_t> present_blocks_i; 
    std::mutex sub_region_mutex;
    clean_lru.push_back(clean_lru_i);
    dirty_lru.push_back(dirty_lru_i);
    stash_set.push_back(stash_set_i);
    present_blocks.push_back(present_blocks_i);
    // sub_regions_mutex_list.push_back(sub_region_mutex);
  }
  uffd_active = true;
  fault_events_queue = new utility::event_queue<utility::fault_event>(num_handling_threads);
  start_handler_thread();
  // printf("Releasing virtual_memory_manager::handler_mutex_global open Thread ID: %ld\n", (uint64_t) syscall(SYS_gettid));
}

void virtual_memory_manager::set_uffd(uint64_t uffd){
  m_uffd = uffd;
}

void * virtual_memory_manager::handler(){
  // std::cout << "Starting handler FUNC in VMM\n";
  // printf("uffd_active %d\n", uffd_active);
  // printf("Starting handler thread with ID %d\n", (uint64_t) syscall(SYS_gettid));
  while (uffd_active){
    /* debug++;
    if (debug % 100000 == 0){
      // printf("In while from thread %ld\n", (uint64_t) syscall(SYS_gettid));
      printf("uffd_active true from thread %ld\n",(uint64_t) syscall(SYS_gettid));
    } */
    // printf("THREAD %ld Checking if Queue empty\n", (uint64_t) syscall(SYS_gettid));
    // if (!fault_events_queue->is_empty()){
      // printf("Waiting on virtual_memory_manager::handler_mutex_global Thread ID: %ld\n", (uint64_t) syscall(SYS_gettid));
      // const std::lock_guard<std::mutex> lock(virtual_memory_manager::handler_mutex_global);
      // printf("Aquired virtual_memory_manager::handler_mutex_global handler Thread ID: %ld\n", (uint64_t) syscall(SYS_gettid));
      // if (!fault_events_queue.empty()){
        // Get and assert faulting address
        // printf("Dequeing from thread %ld \n", (uint64_t) syscall(SYS_gettid));
        utility::fault_event fevent = fault_events_queue->dequeue();
        if (fevent.address == 0){
          printf("Got address zero from thread %ld \n", (uint64_t) syscall(SYS_gettid));
          break;
        }

        
        uint64_t fault_address = fevent.address; // (uint64_t) (msg.arg.pagefault.address); // &~(m_block_size - 1));
        // printf("Handling in VMM from thread %ld for address %ld \n", (uint64_t) syscall(SYS_gettid), fault_address);
        uint64_t start_address = (uint64_t) m_region_start_address;
        uint64_t block_index = (fault_address - start_address) / m_block_size;
        uint64_t block_address = start_address + block_index * m_block_size;
        // std::cout << "BLOCK ADDRESS: " << block_address << std::endl;
        
        // Identify fault type
        bool is_wp_fault = fevent.is_wp_fault;
        bool is_write_fault = fevent.is_write_fault;
        int sub_region_index = block_address % num_handling_threads;
        printf("Hello from thread %ld fault_address %ld block address %ld is_wp_fault %d\n",(uint64_t) syscall(SYS_gettid), fault_address, block_address, (int) is_wp_fault);
        /* is_wp_fault = (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP);
        is_write_fault = ((!is_wp_fault) && (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE)); */
        const std::lock_guard<std::mutex> lock(sub_regions_mutex_list[sub_region_index]);
        if ((std::find(present_blocks[sub_region_index].begin(), present_blocks[sub_region_index].end(), block_address) != present_blocks[sub_region_index].end()) && !is_wp_fault){
          std::cout << "Address found, continuing ...\n";
          continue;
        }
        // Handling
        // std::cout << "Identifier: " << this << std::endl;
        // printf("Starting address: %ld blocks_ids address: %ld Thread ID: %ld\n", (uint64_t) m_region_start_address, (uint64_t) &blocks_ids[0], (uint64_t) syscall(SYS_gettid));
        if (is_wp_fault){
          // printf("Handling WP from thread %ld for address %ld \n", (uint64_t) syscall(SYS_gettid), fault_address);
          if (m_read_only){
            std::cerr << "Privateer Error: write fault on a read-only region" << std::endl;
            exit(-1);
          }
          // std::cout << "WP Fault Being Handled\n";
          // Move from clean_lru to dirty_lru
          clean_lru[sub_region_index].remove((uint64_t) block_address);
          dirty_lru[sub_region_index].push_front((uint64_t) block_address);
          if (stash_set[sub_region_index].find(block_address) != stash_set[sub_region_index].end()){
            // std::cout << "STASHED TO CLEAN TO DIRTY" << std::endl;
            if (!m_block_storage->unstash_block(block_index)){
              std::cerr << "virtual_memory_manager: Error unstashing block with index= " << block_index << std::endl;
              exit(-1);
            }
            stash_set[sub_region_index].erase(block_address);
          }
          // Write-unprotect
          // std::cout << "write-protect fault of present page with address: " << block_address << std::endl;
          struct uffdio_writeprotect uffdio_writeprotect;
          uffdio_writeprotect.range.start = block_address;
          uffdio_writeprotect.range.len = m_block_size;
          uffdio_writeprotect.mode = 0; // UFFDIO_WRITEPROTECT_MODE_DONTWAKE;
          if (ioctl(m_uffd, UFFDIO_WRITEPROTECT, &uffdio_writeprotect) == -1){
              std::cerr << "Error ioctl-UFFDIO_WRITEPROTECT - " << strerror(errno) << std::endl;
              exit(-1);
          }
        }
        else{ // std::cout << "BEFORO Handler 420" << std::endl;
          if (present_blocks[sub_region_index].find(block_address) == present_blocks[sub_region_index].end()){
            // std::cout << "Handler 420" << std::endl;
            evict_if_needed(sub_region_index);
            // Check if backing block exists
            int backing_block_fd = -1;
            std::string backing_block_path = ""; // printf("Handler 424 %d\n", syscall(SYS_gettid)); // std::cout << "Handler 424\n";
            std::string stash_backing_block_path = m_block_storage->get_block_stash_path(block_index); // std::cout << "Handler 425" << std::endl;
            std::string blocks_path = m_block_storage->get_blocks_path(); // std::cout << "Handler 426" << std::endl;
            // std::cout << "Handler 427" << std::endl;
            // std::cout << "Handler 427 DASH NEW" << std::endl;
            // std::cout << blocks_ids[block_index] << std::endl;
            // std::cout << "Handler 427 AFTER PRINT" << std::endl;
            // std::cout << "block_index = " << block_index << std::endl;
            if (!stash_backing_block_path.empty()){ // std::cout << "Handler 427 + 1" << std::endl;
              // std::cout << "Getting block: " << block_index << " from stash " << stash_backing_block_path << std::endl;
              backing_block_path = stash_backing_block_path;
            }  
            else if(blocks_ids[block_index].compare(EMPTY_BLOCK_HASH) != 0){ // std::cout << "Handler 427 + 2" << std::endl;
              // std::cout << "Getting block: " << block_index << " from blocks " << blocks_ids[block_index] << std::endl;
              backing_block_path = m_block_storage->get_block_full_path(block_index, blocks_ids[block_index]) + "/" + blocks_ids[block_index];
            }  // std::cout << "Handler 436" << std::endl;
            bool is_zero_page = false;
            struct uffdio_copy uffdio_copy;
            if (!backing_block_path.empty()){
              // std::cout << "block exists" << std::endl;
              void* temp_buffer =  mmap(nullptr, m_block_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
              if (temp_buffer == MAP_FAILED){
                std::cerr << "Error mmap temp: " << strerror(errno) << std::endl;
                exit(-1);
              }

              // read block content into temporary buffer
              backing_block_fd = open(backing_block_path.c_str(), O_RDONLY);
              if (backing_block_fd == -1){
                std::cerr << "virtual_memory_manager: Error opening backing block: " << backing_block_path  << " for address: " << block_address << " - " << strerror(errno) << std::endl;
                exit(-1);
              }
              #ifdef USE_COMPRESSION
              // std::cout << "USING COMPRESSION DECOMPRESSING" << std::endl;
              // std::cout << "Backing block path: " << backing_block_path.c_str() << std::endl;
              // printf("Starting address: %ld blocks_ids address: %ld Thread ID: %ld", (uint64_t) m_region_start_address, (uint64_t) &blocks_ids[0], (uint64_t) syscall(SYS_gettid));
              size_t compressed_block_size = utility::get_file_size(backing_block_path.c_str());
              void* const read_buffer = malloc(compressed_block_size);
              if (pread(backing_block_fd, read_buffer, compressed_block_size, 0) == -1){
                std::cerr << "virtual_memory_manager: Error reading backing block: " << backing_block_path << " for address: " << " - " << strerror(errno) << block_address << std::endl;
                exit(-1);
              }
              size_t decompressed_size = utility::decompress(read_buffer, temp_buffer, compressed_block_size);
              free(read_buffer);
              #else
              // std::cout << "NOT USING COMPRESSION" << std::endl;
              if (pread(backing_block_fd, temp_buffer, m_block_size, 0) == -1){
                std::cerr << "virtual_memory_manager: Error reading backing block: " << backing_block_path << " for address: " << " - " << strerror(errno) << block_address << std::endl;
                exit(-1);
              }
              #endif
              
              if (::close(backing_block_fd) == -1){
                std::cerr << "virtual_memory_manager: Error closing backing block: " << backing_block_path << " - " << strerror(errno) << std::endl;
                exit(-1);
              }
              
              // struct uffdio_copy uffdio_copy;
              uffdio_copy.src = (unsigned long) temp_buffer;
              uffdio_copy.dst = (unsigned long) block_address;
              uffdio_copy.len = m_block_size;
              uffdio_copy.mode = UFFDIO_COPY_MODE_WP | UFFDIO_COPY_MODE_DONTWAKE;
              uffdio_copy.copy = 0;
              if (ioctl(m_uffd, UFFDIO_COPY, &uffdio_copy) == -1){
                std::cerr << "Error ioctl-UFFDIO_COPY - " << strerror(errno) << std::endl;
                exit(-1);
              }
            }
            else{
              char* tmp_buff;
              if (posix_memalign((void**)&tmp_buff, m_block_size, m_block_size)) {
                std::cerr << "Virtual Memory Manager: Error posix_memalign - " << strerror(errno) << std::endl;
              }
              void *addr = mmap((void *) tmp_buff, m_block_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
              if (addr == MAP_FAILED){
                std::cerr << "Error mmap zero page " << strerror(errno) << std::endl;
                exit(-1);
              }
              // printf("TEMP ADDRESS %ld blocks_ids %ld m_block_size %ld from thread %ld\n", (uint64_t) addr, (uint64_t) &blocks_ids[0], m_block_size , (uint64_t) syscall(SYS_gettid));
              // struct uffdio_copy uffdio_copy;
              uffdio_copy.src = (unsigned long) addr;
              uffdio_copy.dst = (unsigned long) block_address;
              uffdio_copy.len = m_block_size;
              uffdio_copy.mode = UFFDIO_COPY_MODE_WP | UFFDIO_COPY_MODE_DONTWAKE;;
              uffdio_copy.copy = 0;
              if (ioctl(m_uffd, UFFDIO_COPY, &uffdio_copy) == -1){
                std::cerr << "Error ioctl-UFFDIO_COPY for Zero page - " << strerror(errno) << std::endl;
                exit(-1);
              }
              // std::cout << "uffdio_copy.copy = " << uffdio_copy.copy << std::endl;
              is_zero_page = true;
              if (munmap(addr, m_block_size) == -1){
                std::cerr << "Error unmapping temporary buffer - " << strerror(errno) << std::endl;
                exit(-1);
              }
            }
            clean_lru[sub_region_index].push_front(block_address);
            present_blocks[sub_region_index].insert(block_address);
            fault_events_queue->remove_processed(fevent);
            struct uffdio_range uffdio_range;
            uffdio_range.start = block_address;
            uffdio_range.len = m_block_size;
            if (ioctl(m_uffd, UFFDIO_WAKE, &uffdio_range) == -1){
              std::cerr << "Error: ioctl-UFFDIO_WAKE - "   << strerror(errno) << std::endl;
              exit(-1);
            }
          }
        }
      // }
      // printf("Releasing virtual_memory_manager::handler_mutex_global handler Thread ID: %ld\n", (uint64_t) syscall(SYS_gettid));
      
      // printf("DONE Handling in VMM from thread %ld for address %ld \n", (uint64_t) syscall(SYS_gettid), fault_address);
    // }
    // printf("THREAD %ld Queue empty\n", (uint64_t) syscall(SYS_gettid));
  }
  // printf("THREAD %ld QUITTING HERE\n", (uint64_t) syscall(SYS_gettid));
  return NULL;
  // END: Poll for page fault events
  // -------------------------------
}

void* virtual_memory_manager::handler_helper(void *context){
  // printf("Starting Handler HELPER from %ld\n", (uint64_t) syscall(SYS_gettid));
  return ((virtual_memory_manager *)context)->handler();
}

void virtual_memory_manager::evict_if_needed(int sub_region_index){
  void* to_evict;
  if ((present_blocks[sub_region_index].size()*m_block_size) >= m_max_mem_size){
    // std::cout << "EVICTING" << std::endl;
    if (clean_lru[sub_region_index].size() > 0){
      to_evict = (void*) clean_lru[sub_region_index].back();
      // std::cout << "Evicting clean block: " << ((uint64_t) to_evict - (uint64_t) m_region_start_address) / m_block_size << std::endl;
      clean_lru[sub_region_index].pop_back();
    }
    else{
      // std::cout << "I am failing, bye!" << std::endl;
      to_evict = (void*) dirty_lru[sub_region_index].back();
      dirty_lru[sub_region_index].pop_back();
      // std::cout << "Hello from the other side" << std::endl;
      uint64_t block_index = ((uint64_t) to_evict - (uint64_t) m_region_start_address) / m_block_size;
      // std::cout << "stashing block: " << block_index << std::endl;
      if (!m_block_storage->stash_block(to_evict, block_index)){
        std::cerr << "Virtual memory manager: Error stashing block with index: " << block_index << std::endl;
        exit(-1);
      }
      stash_set[sub_region_index].insert((uint64_t) to_evict);
    }
    /* int protect_status = mprotect(to_evict, m_block_size, PROT_NONE);
    if (protect_status == -1){
      std::cerr << "virtual_memory_manager: Error evicting address: " << to_evict << std::endl;
      exit(-1);
    } */
    void *evicted_addr = mmap(to_evict, m_block_size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (evicted_addr == MAP_FAILED){
      std::cerr << "Evict: Error evicting block with address -  " << strerror(errno)<< std::endl;
      exit(-1);
    }
    present_blocks[sub_region_index].erase((uint64_t) to_evict);
  }
}

void virtual_memory_manager::msync(){
  for (int i = 0 ; i < num_handling_threads; i++){
    msync(i);
  }
}

void virtual_memory_manager::msync(int sub_region_index){
  const std::lock_guard<std::mutex> lock(virtual_memory_manager::handler_mutex_global);
  // std::cout << "MSYNC" << std::endl;
  // 1) Write dirty_lru
  // std::cout << "size of dirty LRU: "<< dirty_lru.size() << std::endl;
  std::vector<uint64_t> dirty_lru_vector(dirty_lru[sub_region_index].begin(), dirty_lru[sub_region_index].end());
  #pragma omp parallel for
  for (auto dirty_lru_iterator = dirty_lru_vector.begin(); dirty_lru_iterator != dirty_lru_vector.end(); ++dirty_lru_iterator){
    // std::cout << "storing block\n";
    block_storage block_storage_local(*m_block_storage);
    void* block_address = (void*) *dirty_lru_iterator;
    // if (stash_set.find((uint64_t) block_address) == stash_set.end()){
      uint64_t block_index = ((uint64_t) block_address - (uint64_t) m_region_start_address) / m_block_size;
      bool write_block_fd = true;
      std::string block_hash = block_storage_local.store_block(block_address, write_block_fd, block_index);
      if (block_hash.empty()){
        std::cerr << "virtual_memory_manager: Error storing block with index " << block_index << std::endl;
        exit(-1);
      }
      
      blocks_ids[block_index] = block_hash;// std::string(block_storage_local.get_block_hash(block_fd));
      // Change mprotect to read_only
      /* int mprotect_stat = mprotect(block_address, m_block_size, PROT_READ);
      if (mprotect_stat == -1){
        std::cerr << "virtual_memory_manager: mprotect error for block with address: " << (uint64_t) block_address << " " << strerror(errno) << std::endl;
        exit(-1);
      } */
      
      #pragma omp critical
      {
        // std::cout << "wp-ing block\n";
        struct uffdio_writeprotect uffdio_writeprotect;
        uffdio_writeprotect.range.start = (uint64_t) block_address;
        uffdio_writeprotect.range.len = (uint64_t) m_block_size;
        uffdio_writeprotect.mode = UFFDIO_WRITEPROTECT_MODE_WP; // UFFDIO_WRITEPROTECT_MODE_DONTWAKE;
        if (ioctl(m_uffd, UFFDIO_WRITEPROTECT, &uffdio_writeprotect) == -1){
            std::cerr << "Error ioctl-UFFDIO_WRITEPROTECT" << strerror(errno) << std::endl;
            exit(-1);
        }
        // std::cout << "done wp-ing block\n";
        int sub_region_index = ((uint64_t) block_address) % num_handling_threads;
        clean_lru[sub_region_index].push_front((uint64_t)block_address);
      }
    // }
  }
  for (int i = 0 ; i < num_handling_threads; i++){
    dirty_lru[sub_region_index].clear();
  }
  
  // 2) Commit stashed blocks
  // std::cout << "SIZE OF STASH SET: " << stash_set.size() << std::endl;
  std::vector<uint64_t> stash_vector(stash_set[sub_region_index].begin(), stash_set[sub_region_index].end());
  #pragma omp parallel for
  for (auto stash_iterator = stash_vector.begin(); stash_iterator != stash_vector.end(); ++stash_iterator){
    block_storage block_storage_local(*m_block_storage);
    void* block_address = (void*) *stash_iterator;
    uint64_t block_index = ((uint64_t) block_address - (uint64_t) m_region_start_address) / m_block_size;
    #pragma omp critical
    {
      std::string block_hash = /* block_storage_local.*/ m_block_storage->commit_stash_block(block_index);
      if (block_hash.empty()){
        std::cerr << "virtual_memory_manager: Error committing stash block with address: " << (uint64_t) block_address << std::endl;
        exit(-1);
      }
      blocks_ids[block_index] = block_hash;
    }
  }
  stash_set[sub_region_index].clear();
  update_metadata(sub_region_index);
  // std::cout << "DONE MSYNC" << std::endl;
  struct stat st_dev_null;
  if (fstat(0,&st_dev_null) != 0){
    // std::cout << "Opening /dev/null" << std::endl;
    int dev_null_fd = ::open("/dev/null",O_RDWR);
    // std::cout << "/dev/null FD: " << dev_null_fd << std::endl;
  }
  // std::cout << "DONE MSYNC DEV NULL" << std::endl;
}

// TODO: Redesign and Rewrite
bool virtual_memory_manager::snapshot(const char* version_metadata_path){
  // std::cout << "Snapshot path: " << version_metadata_path  << std::endl;
  // Create new version metadata directory
  if(utility::directory_exists(version_metadata_path)){
    std::cerr << "Error: Version metadata directory already exists" << std::endl;
    return false;
  }

  if (!utility::create_directory(version_metadata_path)){
    std::cerr << "Error: Failed to create version metadata directory at " << version_metadata_path << " - " << strerror(errno) << std::endl;
  }

  // temporarily change metadata file descriptor
  // int temp_metada_fd = metadata_fd;
  std::string snapshot_metadata_path = std::string(version_metadata_path) + "/_metadata";
  std::string m_temp_current_metadata_path = m_version_metadata_path;
  m_version_metadata_path = std::string(version_metadata_path);
  // std::cout << "Privateer: Snapshotting to " << snapshot_metadata_path << std::endl;
  // TODO: Add check or create in a different way
  int metadata_fd = ::open(snapshot_metadata_path.c_str(), O_RDWR | O_CREAT, (mode_t) 0666);
  int close_status = ::close(metadata_fd);
  
  msync();
  m_version_metadata_path = m_temp_current_metadata_path;
  // metadata_fd = temp_metada_fd;

  // Create file to save blocks path
  std::string blocks_path_file_name = std::string(version_metadata_path) + "/_blocks_path";
  std::ofstream blocks_path_file;
  blocks_path_file.open(blocks_path_file_name);
  blocks_path_file << m_block_storage->get_blocks_path();
  blocks_path_file.close();

  // Create file to save current size
  /* std::string size_path_file_name = std::string(version_metadata_path) + "/_size";
  std::ofstream size_path_file;
  size_path_file.open(size_path_file_name);
  size_path_file << m_current_size;
  size_path_file.close(); */


  // Create file to save max. capacity
  std::string capacity_path_file_name = std::string(version_metadata_path) + "/_capacity";
  std::ofstream capacity_path_file;
  capacity_path_file.open(capacity_path_file_name);
  capacity_path_file << m_region_max_capacity;
  capacity_path_file.close();

  return true;
}

void virtual_memory_manager::update_metadata(int sub_region_index){
  // const std::lock_guard<std::mutex> lock(virtual_memory_manager::handler_mutex_global);
  // std::cout << "present_blocks.size(): " << present_blocks.size() << std::endl;
  if (present_blocks[sub_region_index].size() == 0){
    return;
  }
  size_t max_address = *present_blocks[sub_region_index].rbegin();
  size_t current_size = max_address - (uint64_t) m_region_start_address + m_block_size;
  size_t num_blocks = current_size / m_block_size; // m_region_max_capacity / m_block_size;
  // std::cout << "update_metadata() current_size: " << current_size << std::endl;
  // std::cout << "update_metadata() num_blocks:   " << num_blocks << std::endl;
  char* blocks_bytes = new char[num_blocks*HASH_SIZE];
  for (size_t i = 0 ; i < num_blocks ; i++){
    const char* block_hash_bytes = blocks_ids[i].c_str();
    /* if (blocks[i].compare(EMPTY_BLOCK_HASH) != 0){
      current_size = (i+1)*file_granularity;
    } */
    for (int j = 0; j < HASH_SIZE; j++){
      blocks_bytes[i*HASH_SIZE + j] = block_hash_bytes[j];
    }
  }

  std::string metadata_path = m_version_metadata_path + "/_metadata";
  // std::cout << "update metadata to path: " << metadata_path << std::endl;
  int metadata_fd = open(metadata_path.c_str(), O_RDWR);
  if (metadata_fd == -1){
    std::cerr << "virtual_memory_manager: Error opening metadata file " << strerror(errno) << std::endl;
    exit(-1);
  } // printf("METADATA FD: %d THREAD %ld \n",metadata_fd, (uint64_t) syscall(SYS_gettid));
  const auto written = ::pwrite(metadata_fd ,(void*) blocks_bytes, num_blocks*HASH_SIZE, 0);
  if (written == -1){
    std::cerr << "Error, failed to update metadata and mappings: " << strerror(errno) << std::endl;
    exit(-1);
  }
  if (::close(metadata_fd == -1)){
    std::cerr << "Virtual Memory Manager: Error closing metadata file after update - " << strerror(errno) << std::endl;
    // printf("ERROR METADATA FD: %d THREAD %ld ",metadata_fd, (uint64_t) syscall(SYS_gettid));
    exit(-1);
  }
  delete [] blocks_bytes;
}

void* virtual_memory_manager::get_region_start_address(){
  return m_region_start_address;
}

uint64_t virtual_memory_manager::get_block_address(uint64_t fault_address){
  uint64_t start_address = (uint64_t) m_region_start_address;
  uint64_t block_index = (fault_address - start_address) / m_block_size;
  uint64_t block_address = start_address + block_index * m_block_size;
  return block_address;
}

size_t virtual_memory_manager::version_capacity(std::string version_path){
  // Read size path
  std::string size_string;
  std::string size_file_name = std::string(version_path) + "/_capacity";
  std::ifstream size_file;
  size_file.open(size_file_name);
  if (!size_file.is_open()){
    std::cerr << "Error opening size file path at: " << size_file_name << std::endl;
    return (size_t) -1;
  }
  if (!std::getline(size_file, size_string)){
    std::cerr << "Error reading reading file" << std::endl;
    return (size_t) -1;
  }
  size_file.close();
  try {
    size_t size = std::stol(size_string);
    return size;
  }
  catch (const std::invalid_argument& ia){
    std::cerr << "Error parsing version size from file - " << ia.what() << std::endl;
    return (size_t) -1;
  }
}

size_t virtual_memory_manager::current_region_capacity(){
  return m_region_max_capacity;
}

void virtual_memory_manager::create_version_metadata(const char* version_metadata_dir_path, const char* blocks_dir_path, size_t version_capacity, bool allow_overwrite){
  std::string metadata_file_name = std::string(version_metadata_dir_path) + "/_metadata";
  std::string blocks_path_file_name = std::string(version_metadata_dir_path) + "/_blocks_path";
  std::string capacity_file_name = std::string(version_metadata_dir_path) + "/_capacity";

  // Create version directory
  if (utility::directory_exists(version_metadata_dir_path)){
    if (utility::file_exists(metadata_file_name.c_str()) || utility::file_exists(blocks_path_file_name.c_str()) || utility::file_exists(capacity_file_name.c_str())){
      if (allow_overwrite){
        if (!std::filesystem::remove(std::filesystem::path(metadata_file_name)) || !std::filesystem::remove(std::filesystem::path(blocks_path_file_name)) || !std::filesystem::remove(std::filesystem::path(capacity_file_name))){
          std::cerr << "Error removing existing metadata files" << std::endl;
          exit(-1);
        }
        if (!utility::create_directory(version_metadata_dir_path)){
          std::cerr << "Error: Failed to create version metadata directory at " << version_metadata_dir_path << " - " << strerror(errno) << std::endl;
          exit(-1);
        }
      }
      else{
        std::cerr << "Error: Version metadata already exists" << std::endl;
        exit(-1);
      }
    }
  }
  else if (!utility::create_directory(version_metadata_dir_path)){
    std::cerr << "Error: Failed to create version metadata directory at " << version_metadata_dir_path << " - " << strerror(errno) << std::endl;
    exit(-1);
  }
  // Create blocks metadata file
  metadata_fd = ::open(metadata_file_name.c_str(), O_RDWR | O_CREAT | O_EXCL, (mode_t) 0666);
  if (metadata_fd == -1){
    std::cerr << "Privateer: Error opening metadata file: " << metadata_file_name << " - " << strerror(errno) << std::endl;
    exit(-1);
  }
  // Create file to save blocks path
  std::ofstream blocks_path_file;
  blocks_path_file.open(blocks_path_file_name);
  blocks_path_file << blocks_dir_path;
  blocks_path_file.close();

  // Create capacity file
  std::ofstream capacity_file;
  capacity_file.open(capacity_file_name);
  capacity_file << version_capacity;
  capacity_file.close();
}

void virtual_memory_manager::add_page_fault_event(utility::fault_event fevent){
  // printf("Waiting on virtual_memory_manager::add_event_mutex Thread ID: %ld\n", (uint64_t) syscall(SYS_gettid));
  // const std::lock_guard<std::mutex> lock(virtual_memory_manager::add_event_mutex);
  // printf("Aquired virtual_memory_manager::add_event_mutex Thread ID: %ld\n", (uint64_t) syscall(SYS_gettid));
  // printf("Page Fault Event Added to queue for address %ld\n", fevent.address);
  if (!fault_events_queue->found(fevent))
    fault_events_queue->enqueue(fevent);
  // printf("Releasing virtual_memory_manager::add_event_mutex Thread ID: %ld\n", (uint64_t) syscall(SYS_gettid));
}

void virtual_memory_manager::add_page_fault_event_all(utility::fault_event fevent){
  for (int i = 0; i < num_handling_threads; i++){
    add_page_fault_event(fevent);
  }
}

void virtual_memory_manager::start_handler_thread(){
  // std::cout << "Starting handler thread in VMM";
  for (int i = 0; i < num_handling_threads; i++){
    pthread_t fault_handling_thread;
    int status = pthread_create(&fault_handling_thread, NULL, virtual_memory_manager::handler_helper, (void*) this);
    if (status != 0){
      std::cerr << "VMM: Error pthread_create - " << strerror(status) << std::endl;
      exit(-1);
    }
    fault_handling_threads.push_back(fault_handling_thread);
  }
  // printf("Done creating %d handler threads\n",fault_handling_threads.size());
}

void virtual_memory_manager::stop_handler_thread(){
  std::cout << "VMM: stop_handler_thread before pthread_join\n";
  uffd_active = false;
  // std::this_thread::sleep_for (std::chrono::seconds(3));
  /* for (int i = 0; i < num_handling_threads; i++){
    add_page_fault_event({.address = 0, .is_wp_fault = false, .is_write_fault = false});
  } */

  for (int i = 0; i < num_handling_threads; i++){
    printf("VMM: stop_handler_thread before pthread_join for thread %d\n", i);
    int status = pthread_join(fault_handling_threads[i],NULL);
    if (status != 0){
      std::cerr << "VMM: Error pthread_join - " << strerror(status) << std::endl;
      exit(-1);
    }
    printf("VMM: stop_handler_thread after pthread_join for thread %d\n", i);
  }
  std::cout << "VMM: stop_handler_thread after pthread_join\n";
}

int virtual_memory_manager::close(){
  //  << "ByeBye VMM" << std::endl;
  msync();
  std::set<uint64_t>::iterator it;
  // std::cout << "CLOSE BEFORE ITR" << std::endl;
  for (int i = 0; i < num_handling_threads; i++){
    for (it = present_blocks[i].begin(); it != present_blocks[i].end(); ++it) {
      // std::cout << "munmapping address: " << *it << std::endl;
      // std::cout << "munmapping size: " << m_block_size << std::endl;
      void* address = (void*) *it;
      // printf("Munmapping address %ld size %ld thread %ld", (uint64_t) address, (uint64_t) m_block_size, syscall(SYS_gettid));
      int status = munmap(address, m_block_size);
      if (status == -1){
        std::cerr << "virtual_memory_manager: Error unmapping region with address: " << *it << " - " << strerror(errno) << std::endl;
        return -1;
      }
    }
  }
  // std::cout << "CLOSE AFTER ITR" << std::endl;
  delete [] blocks_ids;
  // delete [] blocks_locks;
  delete m_block_storage;
  m_region_start_address = nullptr;
  // std::cout << "CLOSE RETURNING" << std::endl;
  stop_handler_thread();
  return 0;
}

void virtual_memory_manager::deactivate_uffd_thread(){
  // std::cout << "START: deactivate_uffd_thread" << std::endl;
  uffd_active = false;
  /* char bye[5] = "bye";
  write(uffd_pipe[1], bye, 3); */
  // std::cout << "END: deactivate_uffd_thread" << std::endl;
}

size_t virtual_memory_manager::get_block_size(){
  return m_block_size;
}

virtual_memory_manager::~virtual_memory_manager(){
  // std::cout << "Begin VMM dest~"  << std::endl;
  if (close() !=0){
    std::cerr << "virtual_memory_manager: Error, image not closed appropriately" << std::endl;
    exit(-1);
  }
  // std::cout << "VMM closed\n";
  delete fault_events_queue;
  // std::cout << "fault_events_queue deleted\n";
  // std::cout << "VMM dest~ After close" << std::endl;
  // pthread_mutex_destroy(&handler_mutex);
  // std::cout << "VMM dest~ After mutex destroy"<< std::endl;
}

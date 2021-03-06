#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "myMalloc.h"
#include "printing.h"

/* Due to the way assert() prints error messges we use out own assert function
 * for deteminism when testing assertions
 */
#ifdef TEST_ASSERT
  inline static void assert(int e) {
    if (!e) {
      const char * msg = "Assertion Failed!\n";
      write(2, msg, strlen(msg));
      exit(1);
    }
  }
#else
  #include <assert.h>
#endif

/*
 * Mutex to ensure thread safety for the freelist
 */
static pthread_mutex_t mutex;

/*
 * Array of sentinel nodes for the freelists
 */
header freelistSentinels[N_LISTS];

/*
 * Pointer to the second fencepost in the most recently allocated chunk from
 * the OS. Used for coalescing chunks
 */
header * lastFencePost;

/*
 * Pointer to maintian the base of the heap to allow printing based on the
 * distance from the base of the heap
 */ 
void * base;

/*
 * List of chunks allocated by  the OS for printing boundary tags
 */
header * osChunkList [MAX_OS_CHUNKS];
size_t numOsChunks = 0;

/*
 * direct the compiler to run the init function before running main
 * this allows initialization of required globals
 */
static void init (void) __attribute__ ((constructor));

// Helper functions for manipulating pointers to headers
static inline header * get_header_from_offset(void * ptr, ptrdiff_t off);
static inline header * get_left_header(header * h);
static inline header * ptr_to_header(void * p);

// Helper functions for allocating more memory from the OS
static inline void initialize_fencepost(header * fp, size_t object_left_size);
static inline void insert_os_chunk(header * hdr);
static inline void insert_fenceposts(void * raw_mem, size_t size);
static header * allocate_chunk(size_t size);

// Helper functions for freeing a block
static inline void deallocate_object(void * p);

// Helper functions for allocating a block
static inline header * allocate_object(size_t raw_size);

// Helper functions for verifying that the data structures are structurally 
// valid
static inline header * detect_cycles();
static inline header * verify_pointers();
static inline bool verify_freelist();
static inline header * verify_chunk(header * chunk);
static inline bool verify_tags();
static inline void addtolist(header * freelist, int list);
static void init();

static bool isMallocInitialized;

static inline int find_free(size_t size);
/**
 * @brief Helper function to retrieve a header pointer from a pointer and an 
 *        offset
 *
 * @param ptr base pointer
 * @param off number of bytes from base pointer where header is located
 *
 * @return a pointer to a header offset bytes from pointer
 */
static inline header * get_header_from_offset(void * ptr, ptrdiff_t off) {
	return (header *)((char *) ptr + off);
}

/**
 * @brief Helper function to get the header to the right of a given header
 *
 * @param h original header
 *
 * @return header to the right of h
 */
header * get_right_header(header * h) {
	return get_header_from_offset(h, get_object_size(h));
}

/**
 * @brief Helper function to get the header to the left of a given header
 *
 * @param h original header
 *
 * @return header to the right of h
 */
inline static header * get_left_header(header * h) {
  return get_header_from_offset(h, -h->object_left_size);
}

/**
 * @brief Fenceposts are marked as always allocated and may need to have
 * a left object size to ensure coalescing happens properly
 *
 * @param fp a pointer to the header being used as a fencepost
 * @param object_left_size the size of the object to the left of the fencepost
 */
inline static void initialize_fencepost(header * fp, size_t object_left_size) {
	set_object_state(fp,FENCEPOST);
	set_object_size(fp, ALLOC_HEADER_SIZE);
	fp->object_left_size = object_left_size;
}

/**
 * @brief Helper function to maintain list of chunks from the OS for debugging
 *
 * @param hdr the first fencepost in the chunk allocated by the OS
 */
inline static void insert_os_chunk(header * hdr) {
  if (numOsChunks < MAX_OS_CHUNKS) {
    osChunkList[numOsChunks++] = hdr;
  }
}
static void printlist(){
	header * first = get_right_header(base);
	print_object(base);
	print_object(first);
	while (get_object_size(get_right_header(first) )!= 0){
		print_object(get_right_header(first));
		first = get_right_header(first);
	}
	for(int x = 0; x < N_LISTS; x++){
		header * pie = &freelistSentinels[x];
		if(pie-> next != pie){
			print_object(pie);
			print_object(pie->next);
		}
	}
			
}
/**
 * @brief given a chunk of memory insert fenceposts at the left and 
 * right boundaries of the block to prevent coalescing outside of the
 * block
 *
 * @param raw_mem a void pointer to the memory chunk to initialize
 * @param size the size of the allocated chunk
 */
inline static void insert_fenceposts(void * raw_mem, size_t size) {
  // Convert to char * before performing operations
  char * mem = (char *) raw_mem;

  // Insert a fencepost at the left edge of the block
  header * leftFencePost = (header *) mem;
  initialize_fencepost(leftFencePost, ALLOC_HEADER_SIZE);

  // Insert a fencepost at the right edge of the block
  header * rightFencePost = get_header_from_offset(mem, size - ALLOC_HEADER_SIZE);
  initialize_fencepost(rightFencePost, size - 2 * ALLOC_HEADER_SIZE);
}

/**
 * @brief Allocate another chunk from the OS and prepare to insert it
 * into the free list
 *
 * @param size The size to allocate from the OS
 *
 * @return A pointer to the allocable block in the chunk (just after the 
 * first fencpost)
 */
static void remove_list(header * freelist, size_t original_size){
	if(original_size > 496){
//		exit(0);
	}
	header * former = &freelistSentinels[find_free(original_size)];
	while(former -> next != freelist){
		former = former -> next;
	}
	former -> next = freelist -> next;
	former -> next -> prev = former;
	//addtolist(freelist, find_free(get_object_size(freelist)));
}
static header * isCombine(header * freelist){
	//print_object(lastFencePost);
	
	header * delFence2 = get_left_header(freelist);
	header * delFence1 = get_left_header(delFence2);
	header * reset = &freelistSentinels[N_LISTS -1];
	//set_object_state(freelist,1);
	numOsChunks--;
	size_t size = get_object_size(freelist);
	set_object_state(delFence1,0);	
	set_object_size(delFence1,get_object_size(freelist) + 32);
	get_right_header(delFence1) -> object_left_size = get_object_size(delFence1);
		
	addtolist(delFence1,find_free(get_object_size(delFence1)));
	if (verify_pointers() != NULL){
		exit(0);
	}
	lastFencePost = get_right_header(get_right_header(delFence1));
	return get_right_header(delFence1);
	
	
		
	
	
}
static header * combineleft(header * freelist, header * lefto){
	//print_object(freelist->next);
	//print_object(lefto->next);
	//exit(0);
	int flag = 0;
	if(get_object_size(lefto) > 496){
		flag = 1;
	}
	remove_list(freelist,get_object_size(freelist));
	if(flag != 1){
		remove_list(lefto,get_object_size(lefto));
	}
	set_object_size(lefto,get_object_size(lefto) + get_object_size(freelist));
	header * righto = get_right_header(lefto);
	righto-> object_left_size = get_object_size(lefto);
	if(flag != 1){
	addtolist(lefto,find_free(get_object_size(lefto)));
	}
	//printlist();
	return lefto;
}
static header * allocate_block(size_t newsize, header * freelist, int counter){
	//print_object(get_left_header(freelist));
	if(get_object_state(get_left_header(freelist))==UNALLOCATED && get_object_size(get_left_header(freelist)) != 0){
		freelist = combineleft(freelist,get_left_header(freelist));
	}
	header * lol = get_header_from_offset(freelist, get_object_size(freelist) -newsize);
	set_object_size(lol, newsize);
	size_t size = get_object_size(freelist);
	lol->object_left_size = get_object_size(freelist) - newsize;
	set_object_size(freelist,get_object_size(freelist) - newsize);
	//lol-> object_left_size = get_object_size(get_left_header(lol));
	get_right_header(lol) -> object_left_size = newsize;
	set_object_state(lol, ALLOCATED);

	if (counter == 1){
		int nextlist = find_free(get_object_size(freelist));
		if(verify_tags()){
			exit(0);
		}
		header * newfree = &freelistSentinels[nextlist];
		//print_object(newfree);
		
		if(get_object_state(get_right_header(lastFencePost) )!= 2){
			if(get_object_size(freelist) < 496){
				remove_list(freelist,size);
				addtolist(freelist,find_free(get_object_size(freelist)));
			}		
		}
		else{
			
			remove_list(freelist,size);
			isCombine(freelist);
			
		}
			
		//isCombine(freelist);	
		if (verify_freelist() ==false){
			exit(0);
		}
		if(verify_tags()){
			exit(0);
		}
		if(detect_cycles() != NULL){
			exit(0);
		}
		
		//print_object(get_left_header(lol)->next);	
		//print_object(get_left_header(get_left_header(lol))->next);
		//exit(0);
		
		if(get_object_state(get_left_header(get_left_header(lol)))==UNALLOCATED && get_object_size(get_left_header(get_left_header(lol)))!=0){
			//exit(0);
			combineleft(get_left_header(lol),get_left_header(get_left_header(lol)));
			//printlist();
		}
		//print_object(&freelistSentinels[N_LISTS-1]);
		return lol;
	}
	if(get_object_size(freelist) < 496){
		//exit(0);
		remove_list(freelist, size);
		addtolist(freelist,find_free(get_object_size(freelist)));
	}
//	print_object(get_right_header(base));
	if(verify_freelist() ==false){
		exit(0);
	}
	if(detect_cycles() != NULL){
		exit(0);
	}
	header * freelistfinder = &freelistSentinels[5];
	//print_object(freelistfinder);
	//print_object(base);
	return lol ;
}
	
static header * allocate_chunk(size_t size) {
  void * mem = sbrk(size);
  
  insert_fenceposts(mem, size);
  header * hdr = (header *) ((char *)mem + ALLOC_HEADER_SIZE);
  set_object_state(hdr, UNALLOCATED);
  set_object_size(hdr, size - 2 * ALLOC_HEADER_SIZE);
  hdr->object_left_size = ALLOC_HEADER_SIZE;
  return hdr;
}
//hello
/**
 * @brief Helper allocate an object given a raw request size from the user
 *
 * @param raw_size number of bytes the user needs
 *
 * @return A block satisfying the user's request
 */
static inline header * allocate_object(size_t raw_size) {
  // TODO implement allocation
  size_t newsize;
  //if(raw_size == 100){
//	exit(0);
  //}
  if(raw_size == 0){
	return NULL;
  }
  else if(raw_size >= ARENA_SIZE){
	return NULL;
  }
  else if(raw_size < ALLOC_HEADER_SIZE){
	newsize = 2 *ALLOC_HEADER_SIZE;
  }
  else if(raw_size % 8 != 0){
	int diff = raw_size % 8;
	newsize = raw_size + (8-diff) + ALLOC_HEADER_SIZE;
  }
  else{
	newsize = raw_size + ALLOC_HEADER_SIZE;
  } 
  
  //TODO find freelists
  header * curfree = &freelistSentinels[N_LISTS-1];
  header * freelist = curfree -> next;
  int flag = 0;
  int cnt = 0;
  for(int i = 0; i < N_LISTS; i++){
	header * nextFree = &freelistSentinels[i];
	//print_object(nextFree -> next);
	//print_object(nextFree);
	if(nextFree -> prev != nextFree){
		header * counter = nextFree;
		while(counter -> prev != nextFree){
			if(get_object_size((counter -> prev)) >= newsize && flag ==0){
	//			print_object(counter -> prev);
  				curfree = &freelistSentinels[i];
  				freelist = counter->prev;
				flag = 1;
		
			}
			if(get_object_size(counter -> prev) >= 480){
				//print_object(counter -> next);
			}
			counter = counter -> prev;
			
		}
	}
	else{
		if(get_object_size((nextFree -> prev)) >= newsize && flag ==0){
			curfree = &freelistSentinels[i];
			freelist = curfree-> prev;
			flag = 1;
		}
	}
	cnt = i;
   }
   flag = 0;
   //if(cnt = N_LISTS-1){
//	header * free = curfree;
//	while(free -> prev != curfree){
//		if(newsize == get_object_size(free->prev) && flag ==0){
//			freelist = free -> prev;
//			flag = 1;
//		}
//		free = free-> prev;
//	}
   //}
   //print_object(freelist); 
  
  
  //printf("%ld/n",newsize);
  //exit(0);
  //print_object(freelist); 
  if(get_object_size(freelist) == newsize){
	header * newspace = freelist->prev;
	
	//newspace -> next = newspace;
	//newspace -> prev = newspace;
	remove_list(freelist,get_object_size(freelist));
	
	//freelist -> next = get_right_header(freelist);
        //freelist -> prev = get_left_header(freelist);
	set_object_state(freelist,ALLOCATED);	
	set_object_size(freelist, newsize);
	//newspace -> next = newspace;
	//newspace -> prev = newspace;
	return (header *)freelist->data;
  }
  else if(newsize < get_object_size(freelist)){
  	return (header *)allocate_block(newsize, freelist,2)->data;
  }
  else{
//	exit(0);
	//print_object(freelist);
	//printf("\n%ld\n", newsize);
	//exit(0);	
	int nextlist = find_free(get_object_size(freelist));
	//header * testfence = get_right_header(get_right_header(freelist));
	header * block = allocate_chunk(ARENA_SIZE);
	header * firstfence = get_left_header(block);
	insert_os_chunk(firstfence);
	nextlist = find_free(get_object_size(block));
	addtolist(block,nextlist);
	if (verify_pointers()){
		exit(0);
	}	
		
	return (header *)(allocate_block(newsize,block,1)->data);
	
	
	}
	
  
   
  
  
  

}
int find_free(size_t size){
	if (size < 496){
		return size/8 -3;
	}
	else{
		return N_LISTS -1;
	}
}

/**
 * @brief Helper to get the header from a pointer allocated with malloc
 *
 * @param p pointer to the data region of the block
 *
 * @return A pointer to the header of the block
 */
static inline header * ptr_to_header(void * p) {
  return (header *)((char *) p - ALLOC_HEADER_SIZE); //sizeof(header));
}

/**
 * @brief Helper to manage deallocation of a pointer returned by the user
 *
 * @param p The pointer returned to the user by a call to malloc
 */
static inline void addtolist(header * lol, int findfree1){
	header * freelist = &freelistSentinels[findfree1];
	if(freelist -> next != freelist){
		
		header * next1 = freelist -> next;
		freelist -> next = lol;
		lol -> prev = freelist;
		next1 -> prev = lol;
		lol -> next = next1;
	}
	else{
		freelist -> next = lol;
		lol -> prev = freelist;
		freelist -> prev = lol;
		lol -> next = freelist;
	}
}
static inline void deallocate_object(void * p) {
  // TODO implement deallocation
 
  if (p != NULL){
	header * lol = get_header_from_offset((header *)p, -ALLOC_HEADER_SIZE);
	//if(get_object_size(lol) == 128){
	//	exit(0);
	//}
	if(get_object_state(lol) == UNALLOCATED){
		printf("%s\n", "Double Free Detected");
		assert(0);
	}
	int currentfreelist = find_free(get_object_size(lol));
	
	if(get_object_state(get_right_header(lol))!=UNALLOCATED && get_object_state(get_left_header(lol))!=UNALLOCATED){
		set_object_state(lol,UNALLOCATED);
		addtolist(lol,currentfreelist);
		//if(get_object_size(lol) == 128){
	//		exit(0);
		//}
		}
	else if(get_object_state(get_right_header(lol))!=UNALLOCATED && get_object_state(get_left_header(lol))==UNALLOCATED){
		header * lefto = get_left_header(lol);
		size_t left = get_object_size(lefto);
		set_object_state(lol,UNALLOCATED);
		set_object_size(lefto, get_object_size(lol) +get_object_size(lefto));
		get_right_header(lefto) -> object_left_size = get_object_size(lefto);
		
		header * former = &freelistSentinels[find_free(get_object_size(lefto) - get_object_size(lol))];
		while(former -> next != lefto){
			former = former -> next;
		}	
		former -> next = lefto -> next;
		former -> next -> prev = former;
		addtolist(lefto, find_free(get_object_size(lefto)));
		//if(get_object_size(lefto) == 128){
		//	exit(0);
		//}
		//exit(0);
	}
	else if(get_object_state(get_right_header(lol))==UNALLOCATED && get_object_state(get_left_header(lol))!=UNALLOCATED){
		header * righto = get_right_header(lol);
		set_object_state(lol,UNALLOCATED);
		set_object_size(lol,get_object_size(lol) + get_object_size(righto));
		
		addtolist(lol, find_free(get_object_size(lol)));
		header * former = &freelistSentinels[find_free(get_object_size(righto))];
		while(former -> next != righto){
			former = former -> next;
		}
		former -> next = righto -> next;
		former -> next -> prev = former;
		get_right_header(lol) -> object_left_size = get_object_size(lol);	
		//if(get_object_size(lol) == 128){
	//		exit(0);
		//}	
			
	}
	else{
		
		header * righto = get_right_header(lol);
		header * lefto = get_left_header(lol);
		size_t size = get_object_size(lefto);
		set_object_state(lol, UNALLOCATED);
		set_object_size(lefto, get_object_size(lol) + get_object_size(lefto) + get_object_size(righto));
		get_right_header(lefto) -> object_left_size = get_object_size(lefto);
		header * former = &freelistSentinels[find_free(get_object_size(righto))];
		//remove_list(lefto, size);
		//addtolist(lefto,find_free(get_object_size(lefto)));
		while(former -> next != righto){
			former = former -> next;
		}
		former -> next = righto -> next;
		former -> next -> prev = former;
		//if(get_object_size(lefto) == 128){
		//	exit(0);
		//}
	}
}
}

/**
 * @brief Helper to detect cycles in the free list
 * https://en.wikipedia.org/wiki/Cycle_detection#Floyd's_Tortoise_and_Hare
 *
 * @return One of the nodes in the cycle or NULL if no cycle is present
 */
static inline header * detect_cycles() {
  for (int i = 0; i < N_LISTS; i++) {
    header * freelist = &freelistSentinels[i];
    for (header * slow = freelist->next, * fast = freelist->next->next; 
         fast != freelist; 
         slow = slow->next, fast = fast->next->next) {
      if (slow == fast) {
        return slow;
      }
    }
  }
  return NULL;
}

/**
 * @brief Helper to verify that there are no unlinked previous or next pointers
 *        in the free list
 *
 * @return A node whose previous and next pointers are incorrect or NULL if no
 *         such node exists
 */
static inline header * verify_pointers() {
  for (int i = 0; i < N_LISTS; i++) {
    header * freelist = &freelistSentinels[i];
    for (header * cur = freelist->next; cur != freelist; cur = cur->next) {
      if (cur->next->prev != cur || cur->prev->next != cur) {
        return cur;
      }
    }
  }
  return NULL;
}

/**
 * @brief Verify the structure of the free list is correct by checkin for 
 *        cycles and misdirected pointers
 *
 * @return true if the list is valid
 */
static inline bool verify_freelist() {
  header * cycle = detect_cycles();
  if (cycle != NULL) {
    fprintf(stderr, "Cycle Detected\n");
    print_sublist(print_object, cycle->next, cycle);
    return false;
  }

  header * invalid = verify_pointers();
  if (invalid != NULL) {
    fprintf(stderr, "Invalid pointers\n");
    print_object(invalid);
    return false;
  }

  return true;
}

/**
 * @brief Helper to verify that the sizes in a chunk from the OS are correct
 *        and that allocated node's canary values are correct
 *
 * @param chunk AREA_SIZE chunk allocated from the OS
 *
 * @return a pointer to an invalid header or NULL if all header's are valid
 */
static inline header * verify_chunk(header * chunk) {
	if (get_object_state(chunk) != FENCEPOST) {
		fprintf(stderr, "Invalid fencepost\n");
		print_object(chunk);
		return chunk;
	}
	
	for (; get_object_state(chunk) != FENCEPOST; chunk = get_right_header(chunk)) {
		if (get_object_size(chunk)  != get_right_header(chunk)->object_left_size) {
			fprintf(stderr, "Invalid sizes\n");
			print_object(chunk);
			return chunk;
		}
	}
	
	return NULL;
}

/**
 * @brief For each chunk allocated by the OS verify that the boundary tags
 *        are consistent
 *
 * @return true if the boundary tags are valid
 */
static inline bool verify_tags() {
  for (size_t i = 0; i < numOsChunks; i++) {
    header * invalid = verify_chunk(osChunkList[i]);
    if (invalid != NULL) {
      return invalid;
    }
  }

  return NULL;
}

/**
 * @brief Initialize mutex lock and prepare an initial chunk of memory for allocation
 */
static void init() {
  // Initialize mutex for thread safety
  pthread_mutex_init(&mutex, NULL);

#ifdef DEBUG
  // Manually set printf buffer so it won't call malloc when debugging the allocator
  setvbuf(stdout, NULL, _IONBF, 0);
#endif // DEBUG

  // Allocate the first chunk from the OS
  header * block = allocate_chunk(ARENA_SIZE);
  
  header * prevFencePost = get_header_from_offset(block, -ALLOC_HEADER_SIZE);
  insert_os_chunk(prevFencePost);
  
  lastFencePost = get_header_from_offset(block, get_object_size(block));
  
  // Set the base pointer to the beginning of the first fencepost in the first
  // chunk from the OS
  base = ((char *) block) - ALLOC_HEADER_SIZE; //sizeof(header);
  
  
  // Initialize freelist sentinels
  for (int i = 0; i < N_LISTS; i++) {
    header * freelist = &freelistSentinels[i];
    freelist->next = freelist;
    freelist->prev = freelist;
  }
 
  // Insert first chunk into the free list
  header * freelist = &freelistSentinels[N_LISTS - 1];
  freelist->next = block;
  freelist->prev = block;
  block->next = freelist;
  block->prev = freelist;
}

/* 
 * External interface
 */
void * my_malloc(size_t size) {
  pthread_mutex_lock(&mutex);
  header * hdr = allocate_object(size); 
  pthread_mutex_unlock(&mutex);
  return hdr;
}

void * my_calloc(size_t nmemb, size_t size) {
  return memset(my_malloc(size * nmemb), 0, size * nmemb);
}

void * my_realloc(void * ptr, size_t size) {
  void * mem = my_malloc(size);
  memcpy(mem, ptr, size);
  my_free(ptr);
  return mem; 
}

void my_free(void * p) {
  pthread_mutex_lock(&mutex);
  deallocate_object(p);
  pthread_mutex_unlock(&mutex);
}

bool verify() {
  return verify_freelist() && verify_tags();
}

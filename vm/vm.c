/* vm.c: Generic interface for virtual memory objects. */

#include "bitmap.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "vm/vm.h"
#include "vm/inspect.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	frame_init();
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

enum vm_type
page_is_stack (struct page *page) {
	int ty = IS_STACK (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return IS_STACK (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page); //spt 넘겨주게 바꾸고싶음.
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	void * va = pg_round_down(upage);
	if(spt_find_page (spt, va) != NULL) {
		goto err;
	}
	/* TODO: Create the page, fetch the initialier according to the VM type,
	* TODO: and then create "uninit" page struct by calling uninit_new. */ 
	struct page *page = (struct page *)calloc(sizeof(struct page),1); //calloc 에러처리?
	
	bool (*initializer)(struct page *, enum vm_type, void *kva);
	switch (VM_TYPE(type)){
		case VM_ANON:
			initializer = (bool (*)(struct page *, enum vm_type, void *kva))anon_initializer;
			break;
		case VM_FILE:
			initializer = (bool (*)(struct page *, enum vm_type, void *kva))file_backed_initializer;
			break;
		default:
			PANIC("invalid page type");
			break;
	}
	uninit_new(page, va, init, type, aux, initializer);
	/* TODO: You should modify the field after calling the uninit_new. */
	page->writable = writable;
	page->pml4 = thread_current()->pml4;
	/* TODO: Insert the page into the spt. */
	if(!spt_insert_page(spt,page)){
		goto err;
	}
	return true;
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	//struct page *page = NULL;
	/* TODO: Fill this function. */
	struct hash *pages = &spt->pages;
	struct page p;
  	struct hash_elem *e;
	va = pg_round_down(va);
  	p.va = va;
  	e = hash_find (pages, &p.hash_elem);
  	return e != NULL ? hash_entry (e, struct page, hash_elem) : NULL;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt,
		struct page *page) {
	int succ = false;
	struct hash *pages = &spt->pages;
	/* TODO: Fill this function. */
	struct hash_elem *elem = hash_insert (pages, &page->hash_elem);
	if(!elem){
		succ = true;
	}
	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	struct hash *pages = &spt->pages;
	hash_delete(pages, &page->hash_elem);
	page->frame->page = NULL; //공유에서 변경
	vm_dealloc_page (page);
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	/* TODO: The policy for eviction is up to you. */
	
	//OSTEP p269 clock algorithm
	struct hash *h = &frame_table.frames;
	struct hash_iterator *i = &frame_table.clock_hand;
	struct frame *frame;
	struct page *page;

	bool use_bit;
	do {
		frame = hash_entry (hash_cur (i), struct frame, hash_elem);
		page = frame->page;
		use_bit = pml4_is_accessed(page->pml4, page->va);
		if(use_bit){
			pml4_set_accessed(page->pml4, page->va, false);
		}else{
			return page->frame;
		}
	} while (hash_next (i));
	for(int j = 0; j < 2; j++){
		hash_first(i, h);
		while (hash_next (i)) {
			frame = hash_entry (hash_cur (i), struct frame, hash_elem);
			page = frame->page;
			use_bit = pml4_is_accessed(page->pml4, page->va);
			if(use_bit){
				pml4_set_accessed(page->pml4, page->va, false);
			}else{
				return page->frame;
			}
		}
	}
	PANIC('cannot find victim with clock algorithm\n');
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */
	swap_out(victim->page);
	return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: Fill this function. */
	
	// bitmap_dump(frame_table.map);

	//빈 프레임 없으면 evict
	if(bitmap_all(frame_table.map,0,frame_table.frame_cnt)){
		frame = vm_evict_frame();
		return frame;
	}else{ //기존 프레임 중 빈 프레임 찾기
		size_t frame_no = bitmap_scan(frame_table.map,0,1,false);
		// printf("frame_no : %d\n", frame_no);
		if(frame_no == BITMAP_ERROR){
			PANIC('bitmap fulled\n');
		}
		struct hash *frames = &frame_table.frames;
		struct frame f;
		struct hash_elem *e;
		f.frame_no = frame_no;
		e = hash_find(frames, &f.hash_elem);
		if(!e){
			PANIC('cannot find frame element\n');
		}
  		return hash_entry (e, struct frame, hash_elem);
	}
	
	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr) {
	void *va = pg_round_down(addr);
	if(!vm_alloc_page(VM_ANON|VM_MARKER_0, va, true)){
		printf("cannot grow stack more\n");
		exit(-1);
	}
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
	return false;
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr,
		bool user , bool write , bool not_present) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = NULL;

	void *va = pg_round_down(addr);

	uintptr_t *rsp = f->rsp;
	if(!user){
		rsp = thread_current()->intr_rsp;
	}
		
	/* TODO: Validate the fault */
	// printf("U_STK %p\n", USER_STACK);
	// printf("n_p: %d, w: %d, u: %d \n", not_present, write, user);
	// printf("addr: %p va:   %p \n", addr, va);
	// printf("rsp   %p rsp d %p \n", rsp, pg_round_down(rsp));
	/* TODO: Your code goes here */
	if(!addr){
		// printf("null address\n");
		return false;
	}
	if(user && is_kernel_vaddr(addr)){
		// printf("kernel memory access on user\n");
		return false;
	}
	if(!not_present){
		printf("why? fault\n");
		return false;
	}
	
	uintptr_t *push_rsp = rsp - 8;
	//not present
	if(USER_STACK - (1<<20) <= push_rsp && push_rsp <= addr && addr <= USER_STACK){
		vm_stack_growth(addr);
	}else if(USER_STACK - (1<<20) <= rsp && rsp <= addr && addr <= USER_STACK){ //stack growth 경우인 경우, 크기 제한
		vm_stack_growth(addr);
	}

	page = spt_find_page(spt, addr);
	if(!page){
		// printf("cannot find page\n");
		return false;
	}

	if(write && !page->writable){
		// printf("write access to r/o page\n");
		return vm_handle_wp(page);
	}

	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va) {
	struct page *page = NULL;
	struct supplemental_page_table *spt = &thread_current ()->spt;
	/* TODO: Fill this function */
	void *page_va = pg_round_down(va);
	if((page = spt_find_page(spt, page_va)) == NULL){
		return false;
	}
	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	if(!pml4_set_page(page->pml4, page->va, frame->kva, page->writable)){
		return false;
	}
	// printf("frame_no %d\n", frame->frame_no);
	bitmap_mark(frame_table.map,frame->frame_no);
	// printf("do claim. frame_table. \n");
	// bitmap_dump(frame_table.map);

	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init (&spt->pages, page_hash, page_less, NULL);
	return;
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src) {
	
	struct page *origin;
	struct page *copy;

	struct hash *h = &src->pages;
	struct hash_iterator i;
	hash_first (&i, h);
	while (hash_next (&i)) {
		origin = hash_entry (hash_cur (&i), struct page, hash_elem);
		copy = (struct page*)calloc(1,sizeof(struct page));
		if(!copy){
			return false;
		}
		memcpy(copy, origin, sizeof(struct page));
		copy->pml4 = thread_current()->pml4;
		hash_insert(&dst->pages, &copy->hash_elem);
		//struct page *inserted_page = spt_find_page(src, copy->va);
		if(copy->frame)
		{
			//공유 구현 시 아래 코드, frame의 page를 리스트로
			// if(!pml4_set_page(thread_current()->pml4, 
			// 	copy->va, inserted_page->frame->kva, 
			// 	inserted_page->writable)){
			// 		return false;
			// }
			// frame_add_page()
			if(!vm_do_claim_page(copy)){
				return false;
			}
			memcpy(copy->frame->kva, origin->frame->kva, PGSIZE);
			
		}
	}
	return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	hash_clear(&spt->pages,hash_page_destroy);
}

void hash_page_destroy(struct hash_elem *e, void *aux){
	const struct page *p = hash_entry (e, struct page, hash_elem);
	//swap, filebacked 처리
	if(page_get_type(p)==VM_FILE){
		swap_out(p);
	}
	vm_dealloc_page(p);
}


uint64_t
page_hash (const struct hash_elem *p_, void *aux UNUSED) {
  const struct page *p = hash_entry (p_, struct page, hash_elem);
  return hash_bytes (&p->va, sizeof p->va);
}

bool
page_less (const struct hash_elem *a_,
           const struct hash_elem *b_, void *aux UNUSED) {
  const struct page *a = hash_entry (a_, struct page, hash_elem);
  const struct page *b = hash_entry (b_, struct page, hash_elem);

  return a->va < b->va;
}

void frame_init(){
	hash_init(&frame_table.frames,frame_hash,frame_less, NULL);
	frame_table.frame_cnt = TOTAL_FRAMES;

	struct frame *frame = NULL;
	for(int i = 0; i < frame_table.frame_cnt; i++){
		frame = (struct frame *)calloc(sizeof(struct frame),1);
		if(!frame){
			PANIC("cannot malloc frame struct\n");
		}
		void *kva = palloc_get_page(PAL_USER|PAL_ZERO);
		if(!kva){
			PANIC("cannot make frame\n");
		}
		frame->frame_no = i;
		frame->kva = kva;
		if(hash_insert(&frame_table.frames,&frame->hash_elem)!=NULL){
			PANIC("frame_no already exist\n");
		}
	}

	hash_first(&frame_table.clock_hand, &frame_table.frames);
	hash_next(&frame_table.clock_hand);
	frame_table.map = bitmap_create(frame_table.frame_cnt);
}

uint64_t
frame_hash (const struct hash_elem *p_, void *aux UNUSED) {
  const struct frame *p = hash_entry (p_, struct frame, hash_elem);
  return hash_bytes (&p->frame_no, sizeof p->frame_no);
}

bool
frame_less (const struct hash_elem *a_,
           const struct hash_elem *b_, void *aux UNUSED) {
  const struct frame *a = hash_entry (a_, struct frame, hash_elem);
  const struct frame *b = hash_entry (b_, struct frame, hash_elem);

  return a->frame_no < b->frame_no;
}

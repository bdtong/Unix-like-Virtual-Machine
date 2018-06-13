#include <unistd.h> //Temporary to see if write is workin
#include <iostream> // Debug
#include <stdio.h>	// Debug
#include <fcntl.h>
#include <cstdio>
#include <stdlib.h> // For Malloc
#include <cstring>
#include <cmath>
#include <vector>
#include <queue>
#include <unordered_map>
#include "VirtualMachine.h"
#include "Machine.h"
//hello
using namespace std;


extern "C" {

class Cluster;
void removeSpaces(char* s);
const char* format83(const char* s);

TVMStatus findAll(int firstCluster, uint32_t size, vector<Cluster*> *clusters );
TVMStatus freeCluster(uint16_t* FATIndex, Cluster* dataCluster);
TVMStatus indexCluster(uint16_t index, Cluster* dataCluster);
TVMStatus findCluster(uint16_t index, int jumps, Cluster* dataCluster );

TVMMainEntry VMLoadModule(const char *module);
TVMStatus VMDateTime(SVMDateTimeRef curdatetime);
TVMStatus VMFileSystemValidPathName(const char *name);
TVMStatus VMFileSystemIsRelativePath(const char *name);
TVMStatus VMFileSystemIsAbsolutePath(const char *name);
TVMStatus VMFileSystemGetAbsolutePath(char *abspath, const char *curpath, const char *destpath);
TVMStatus VMFileSystemPathIsOnMount(const char *mntpt, const char *destpath);
TVMStatus VMFileSystemDirectoryFromFullPath(char *dirname, const char *path);
TVMStatus VMFileSystemFileFromFullPath(char *filename, const char *path);
TVMStatus VMFileSystemConsolidatePath(char *fullpath, const char *dirname, const char *filename);
TVMStatus VMFileSystemSimplifyPath(char *simpath, const char *abspath, const char *relpath);
TVMStatus VMFileSystemRelativePath(char *relpath, const char *basepath, const char *destpath);



// **************************** General Global Variables **********************************************
// Tick length
int ticksms;
volatile unsigned int sleepcycles;
useconds_t usecs;
volatile TVMTick tickcount = 0;
volatile int tids = 0;
volatile unsigned int mids = 0;

// Shared Memory Starting Ptr & Size
 TVMMemorySize SMSize = 0;
 void * SMStart;
volatile unsigned int memids = 0;
const TVMMemoryPoolID VM_MEMORY_POOL_ID_SYSTEM = 1;

// FAT16 File System Start
volatile unsigned int fdcount = 3;
volatile unsigned int ddcount = 3;
volatile unsigned int FAT_IMAGE_FILE_DESCRIPTOR = 0;
TVMMutexID SECTOR_MUTEX = -1;
TVMMutexID FILE_SYSTEM_MUTEX = -1;
unsigned int ROOT_DIR_DD = -1;
void* SHARED_MEM_SECTOR;

// This is the PRIMARY FAT. You can write to BOTH FATs later to be consistent.
// Only write when you dismount the image.
uint16_t* FAT = NULL;
// vector<uint16_t> FAT;
vector<uint8_t> ROOT;
// ****************************************************************************************************


class TCB{
public:

	TCB(){
		tid = tids++;
		result = -1;
		cycles = 0;
		sleeping = false;
	}

	TCB(TVMThreadEntry e, void* p, TVMThreadPriority pr){
		TMachineSignalState sigstate;
		MachineSuspendSignals(&sigstate);

		// stackaddr = malloc(ssize);
		// stacksize = ssize;

		state = VM_THREAD_STATE_DEAD;
		entry = e;
		param = p;
		prio =  pr;
		tid = tids++;

		result = -1;
		cycles = 0;
		sleeping = false;
		MachineResumeSignals(&sigstate);
	}

	~TCB(){
		free(stackaddr);
	}

	SMachineContextRef getMCR(){ return &MC; }
	void* getStack(){ return stackaddr; }
	TVMMemorySize getSSize(){ return stacksize; }
	TVMStatus getState(){ return state; }
	TVMThreadPriority getPriority(){ return prio; }
	TVMThreadID getTID(){ return tid; }
	TVMThreadEntry getEntry(){ return entry; }
	void* getParam(){ return param; }
	int getCycles(){ return cycles; }
	int getResult(){ return result; }

	void setMC(){ 
		MachineContextSave(&MC);
			// cout << "Successfully saved main context" << endl;
	}
	void setState( TVMThreadState s ){ state = s; }
	void setPriority( TVMThreadPriority p){ prio = p; }
	void setCycles( int c ){ cycles = c; }
	void setResult( int r ){ result = r; }

	TVMStatus setStack( TVMMemorySize ssize ){
		TMachineSignalState sigstate;
		MachineSuspendSignals(&sigstate);


		TVMStatus stat = VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SYSTEM, ssize, &stackaddr );
		if(stat == VM_STATUS_SUCCESS){
			stacksize = ssize;
		}

		MachineResumeSignals(&sigstate);
		return stat;
	}

	void decCycles(){ cycles--; }
	void addFile(int fd){ files.push_back(fd); }
	void deleteFile(int fd){
		for(vector<int>::iterator it = files.begin(); it != files.end(); it++){
			if( (*it) == fd ){
				files.erase(it);
				break;
			}
		}
	}

	bool getSleeping(){ return sleeping; }
	void setSleeping(bool s){ sleeping = s; }

	TVMMemorySize getreqLength(){ return reqLength; }
	void setreqLength( TVMMemorySize s){ reqLength = s; }


private:
	SMachineContext MC;

	void* stackaddr;
	TVMMemorySize stacksize;

	TVMStatus state;
	TVMThreadPriority prio;
	TVMThreadID tid;
	TVMThreadEntry entry;
	void* param;

	// Calldata might be tid instead, idk
	void* calldata;
	volatile int result;
	volatile unsigned int cycles;
	bool sleeping;
	vector<int> files;

	vector<unsigned int> mutexIds;

	TVMMemorySize reqLength;
};

// A comparison class to be used with a priority queue for ready TCBs
// http://stackoverflow.com/questions/19535644/how-to-use-the-priority-queue-stl-for-objects
class ComparePriority{	
	// Returns true of t1 < t2, aka. lhs < rhs
public:
	bool operator()(TCB* const t1, TCB* const t2){
		if( t1->getPriority() < t2->getPriority() )
			return true;
		return false;
	}
};

class Mutex{
public:
	Mutex() {
		acquiredThread = NULL;
		mutex = mids++;
		setLock(true);
	}

	void setLock(bool boolean) { lock = boolean; } //not sure if this is needed 
	int getLock() { return lock; }

	void setAcquiredThread(TCB* next) { 
			acquiredThread = next; //next in queue will be the new aquiredThread
			// Note from Tuan: No, it's not the next in queue. Its whats passed in.
	}

	// This was TVMThreadID before, need to see if changing to TCB* return type changes anything
	TCB* getAcquiredThread() { return acquiredThread; }

	TVMThreadID getTID(){ return acquiredThread->getTID(); }

	void addToQueue(TCB* thread) {
			waitList.push(thread);
	}

	TVMMutexID getMutexID() { return mutex; }

	bool checkQueueEmpty() {
		if (waitList.empty())
			return true;
		return false;
	}

	TCB* getNextThread() { return waitList.top(); }

	void DequeueThread(){
		if( !waitList.empty() )
			waitList.pop();
	}

private:

	bool lock; //does aquiredThread posses the mutex lock: true = no, false = yes
	//TCB* originally TVMThreadID 
	TCB* acquiredThread; //ID of thread with aquisiton of mutex

	TVMMutexID mutex; //unsigned int

	priority_queue< TCB *, vector<TCB *>, ComparePriority > waitList; //threads waiting for aquisition of the mutex

};

class Cluster{
public:

	Cluster(int i){
		index = i;
		dirty = false;
	}

	bool dirty;
	uint8_t data[1024];
	int index;
};

class BPB{
public:
	uint8_t NumFATs;
	uint8_t SecPerClus;

	uint16_t Rsvd_SecCnt;
	uint16_t FATSz16;
	uint16_t RootEntCnt;
	uint16_t TotSec16;

	uint32_t TotSec32;

	int FirstRootSector;
	int RootDirectorySectors;
	int FirstDataSector;
	int ClusterCount;
};

BPB BPBSector;

class File{
public:
	File(uint8_t* dirInfo, int f, const char* filename, int i){
		rootOffset = i;
		fd = fdcount++;
		dirty = false;
		openCount = 1;
		flags = f;

		FstClusLO = *(uint16_t*)(dirInfo + 26);
		info.DSize = *(uint32_t*)(dirInfo + 28);

		char fname[13];
		strcpy( fname, filename );
		removeSpaces(fname);
		memcpy( (uint8_t*)info.DShortFileName, (uint8_t*)fname, 13);



		findAll(FstClusLO, info.DSize, &clusters);

		memcpy( (uint8_t*)&info.DAttributes, ((uint8_t*)(dirInfo) + 11), 1);
		curCluster = offset = 0;
		if(flags & O_TRUNC)
			info.DSize = 0;
		if( flags & O_APPEND){
			offset = info.DSize%1024;
			for(vector<Cluster*>::iterator it = clusters.begin(); it != clusters.end(); it++){
				if( it++ == clusters.end() ){
					curCluster = (*it)->index;
					break;
				}
				else
					it--;
			}
		}

		uint16_t buff = *(uint16_t*)(dirInfo + 14);
		info.DCreate.DHour = buff >> 11;
		info.DCreate.DMinute = (buff >> 5) & 0x3F;
		info.DCreate.DSecond =  (buff & 0x1F) << 1; // Have to shift one over, this is a 2 second count-- we want actual count, so multiply by 2
		info.DCreate.DHundredth = (*(dirInfo + 13)/100)%100;

		buff = *(uint16_t*)(dirInfo + 16);
		info.DCreate.DYear = (buff >> 9) + 1980;
		info.DCreate.DMonth = (buff >> 5) & 0x0F;
		info.DCreate.DDay = buff & 0x1F;

		buff = *(uint16_t*)(dirInfo+18);
		info.DAccess.DYear = (buff >> 9) + 1980;
		info.DAccess.DMonth = (buff >> 5) & 0x0F;
		info.DAccess.DDay = buff & 0x1F;

		buff = *(uint16_t*)(dirInfo+22);
		info.DModify.DHour = buff >> 11;
		info.DModify.DMinute = (buff >> 5) & 0x3F;
		info.DModify.DSecond =  (buff & 0x1F) << 1;
		info.DModify.DHundredth = 0;

		buff = *(uint16_t*)(dirInfo + 24);
		info.DModify.DYear = (buff >> 9) + 1980;
		info.DModify.DMonth = (buff >> 5) & 0x0F;
		info.DModify.DDay = buff & 0x1F;
	}

	File(const char* filename, int f, uint8_t* freeDir, int i){
		rootOffset = i;
		fd = fdcount++;
		dirty = true;
		openCount = 1;
		flags = f;

		char fname[13];
		strcpy( fname, filename );
		removeSpaces(fname);
		memcpy( (uint8_t*)info.DShortFileName, (uint8_t*)fname, 13);
		const char* dirCopy = format83(filename);
		memcpy( freeDir, dirCopy, 11);

		SVMDateTime temp;
		VMDateTime(&temp);


		info.DModify = info.DCreate = temp;
		info.DSize = 0;

		uint8_t tempAttr = 0x00;

		if( f & O_RDONLY)
			tempAttr = tempAttr | O_RDONLY;

		memcpy( (uint8_t*)&info.DAttributes, &tempAttr, 1);

		Cluster* tempClus = new Cluster(0);
		freeCluster( &FstClusLO, tempClus);
		clusters.push_back(tempClus);
		
		curCluster = 0;
		offset = 0;
	}

	~File(){
		delete firstCluster;
	}

	int rootOffset;
	int fd;
	bool dirty;
	int openCount;
	int flags;
	uint16_t FstClusLO;
	Cluster* firstCluster; //Dont think I ever use this..
	vector<Cluster *> clusters;
	unsigned int curCluster;
	int offset;
	SVMDirectoryEntry info;
};

class Directory{
public:
	Directory(const char* dirName, uint8_t* dirInfo){
		isRoot = false;
		dd = ddcount++;

		FstClusLO = *(uint16_t*)(dirInfo + 26);
		info.DSize = *(uint32_t*)(dirInfo + 28);

		char dname[13];

		strcpy( dname, dirName );
		removeSpaces(dname);

		memcpy( (uint8_t*)info.DShortFileName, (uint8_t*)dname, 13);
		memcpy( (uint8_t*)info.DLongFileName, (uint8_t*)dname, 13);


		findAll(FstClusLO, info.DSize, &clusters);
		curCluster = offset = 0;

		memcpy( (uint8_t*)&info.DAttributes, ((uint8_t*)(dirInfo) + 11), 1);

		uint16_t buff = *(uint16_t*)(dirInfo + 14);
		info.DCreate.DHour = buff >> 11;
		info.DCreate.DMinute = (buff >> 5) & 0x3F;
		info.DCreate.DSecond =  (buff & 0x1F) << 1; // Have to shift one over, this is a 2 second count-- we want actual count, so multiply by 2
		info.DCreate.DHundredth = (*(dirInfo + 13)/100)%100;

		buff = *(uint16_t*)(dirInfo + 16);
		info.DCreate.DYear = (buff >> 9) + 1980;
		info.DCreate.DMonth = (buff >> 5) & 0x0F;
		info.DCreate.DDay = buff & 0x1F;

		buff = *(uint16_t*)(dirInfo+18);
		info.DAccess.DYear = (buff >> 9) + 1980;
		info.DAccess.DMonth = (buff >> 5) & 0x0F;
		info.DAccess.DDay = buff & 0x1F;

		buff = *(uint16_t*)(dirInfo+22);
		info.DModify.DHour = buff >> 11;
		info.DModify.DMinute = (buff >> 5) & 0x3F;
		info.DModify.DSecond =  (buff & 0x1F) << 1;
		info.DModify.DHundredth = 0;

		buff = *(uint16_t*)(dirInfo + 24);
		info.DModify.DYear = (buff >> 9) + 1980;
		info.DModify.DMonth = (buff >> 5) & 0x0F;
		info.DModify.DDay = buff & 0x1F;

		pathname = dirName;
	}

	Directory(){
		isRoot = true;
		pathname = "/";
		// cerr << "Pathname: " << pathname << endl;;
		ROOT_DIR_DD = dd = ddcount++;
		FstClusLO = (BPBSector.RootEntCnt * 32)/512;

	}

	bool isRoot;
	string pathname;
	int dd;
	int openCount = 1;
	uint16_t FstClusLO;
	unsigned int curCluster;
	int offset;
	vector<Cluster*> clusters;
	SVMDirectoryEntry info;
};


// *********************** Globals dependent on TCB/Mutex/Priority class declarations ************************************
// Global Mutex Vector
vector< Mutex* > mutexes;
// Global TCB Vector
vector<TCB *> tcbs;
// Global File vector
vector<int> files;
Directory * currentDirectory;
vector<File* > fileSys;
vector<Directory* > dirSys;
// Ready queue
priority_queue< TCB *, vector<TCB *>, ComparePriority> rdyQ;
// Waiting queue
vector< TCB *> waiting;
// Current TCB
TCB* ctcb;
// ****************************************************************************************************

// Search for a File within Global Vector
int searchFile(int fd){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	for(unsigned int i = 0; i < fileSys.size(); i++){
		if( fileSys[i]->fd == fd ){
			MachineResumeSignals(&sigstate);
			return i;
		}
	}
	MachineResumeSignals(&sigstate);
	return -1;
}

int searchDirectory(int dd){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	for(unsigned int i = 0; i < dirSys.size(); i++){
		if( dirSys[i]->dd == dd ){
			MachineResumeSignals(&sigstate);
			return i;
		}
	}
	MachineResumeSignals(&sigstate);
	return -1;
}

int searchDirectoryFilename(const char* filename){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	for(unsigned int i = 0; i < dirSys.size(); i++){
		if( !strcmp(dirSys[i]->pathname.c_str(), filename) ){
			MachineResumeSignals(&sigstate);
			return i;
		}
	}
	MachineResumeSignals(&sigstate);
	return -1;
}

int searchFilename(const char* filename){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	for(unsigned int i = 0; i < fileSys.size(); i++){
		if( !strcmp(fileSys[i]->info.DShortFileName, filename) ){
			MachineResumeSignals(&sigstate);
			return i;
		}
	}
	MachineResumeSignals(&sigstate);
	return -1;
}

class MemChunk{
public:

	MemChunk(void * cb, TVMMemorySize l ){
		merged = false;
		length = l;
		chunkBase = cb;
	}

	~MemChunk(){
	}

	void* getChunkBase(){ return chunkBase; }
	void setChunkBase( void* cb ){ chunkBase = cb; }

	TVMMemorySize getLength(){ return length; }
	void setLength(TVMMemorySize len){ length = len; }

	bool getMerged(){ return merged; }
	void setMerged( bool b ){ merged = b; }

	bool operator==( MemChunk& rhs ){
		return this->getChunkBase() == rhs.getChunkBase();
	}

private:
	void * chunkBase;
	TVMMemorySize length;
	bool merged;
};

class MemPool{
public:
	MemPool(void* base, TVMMemorySize l){
		// cerr << "New Memory Pool Created, base: " << base <<" length: " << l << endl;
		baseptr = base;
		length = l;
		memId = memids++;

		MemChunk* temp = new MemChunk(base, l);
		available.push_back(temp);
	}

	~MemPool(){
		for(vector<MemChunk*>::iterator it = available.begin(); it != available.end(); ){
			delete *it;
			it = available.erase(it);
		}
	}

	TVMMemoryPoolID getMemID(){ return memId; } //return memId

	void* firstAvail( TVMMemorySize length ){
		TMachineSignalState sigstate;
		MachineSuspendSignals(&sigstate);

		length = (( length+63)/64) * 64;

		for(vector<MemChunk* >::iterator it = available.begin(); it != available.end(); it++){
			if( length <= (*it)->getLength()  ){
				MachineResumeSignals(&sigstate);
				return reserveMemory(it, length);
			}
		}
		
		MachineResumeSignals(&sigstate);
		return NULL;
	}

	// Purpose: To find the length of the block which remains after allocating a 
	// block of memory to a thread.
	TVMMemorySize findLength( void * base ){
		TMachineSignalState sigstate;
		MachineSuspendSignals(&sigstate);

		TVMMemorySize difference;

		// The length of the memory chunk starts at the largest size possible (base to end of memory)
		TVMMemorySize len = length - ( static_cast<uint8_t*>(base) - static_cast<uint8_t*>(baseptr) );

		// The actual length is the smallest gap possible between the baseptr and an allocated spot in memory
		for(vector<MemChunk*>::iterator it = allocated.begin(); it != allocated.end(); it++){
			if( (difference = static_cast<uint8_t*>( (*it)->getChunkBase() ) - static_cast<uint8_t*>(base))
				&& (difference >= 0 )
				&& (difference < len) )
				len = difference;
		}

		MachineResumeSignals(&sigstate);
		return len;
	}

	MemChunk* isFree(void* m){
		TMachineSignalState sigstate;
		MachineSuspendSignals(&sigstate);
		// In this case Im only going to check the pointers.
		// Overloaded == operator just means that both memory pools point to the same location
		for(vector<MemChunk* >::iterator it = available.begin(); it != available.end(); it++){
			if( (*it)->getChunkBase() < m )
				if( (  static_cast<uint8_t*>((*it)->getChunkBase()) + (*it)->getLength() ) > m ){
					MachineResumeSignals(&sigstate);
					return *it; 
				}
		}
		MachineResumeSignals(&sigstate);
		return NULL;
	}

	void* reserveMemory( vector<MemChunk *>::iterator it, TVMMemorySize length ){
		TMachineSignalState sigstate;
		MachineSuspendSignals(&sigstate);

		MemChunk* temp = *it;

		// Prep a new remaining chunk
		void * newChunk = static_cast<uint8_t*>( (*it)->getChunkBase() ) + length;

		// Check if I can or have to make a new chunk
		if( isFree(newChunk) ){
			TVMMemorySize newChunkLen = findLength( newChunk); 
			// TVMMemorySize newChunkLen = findLength( (*it)->getChunkBase(), newChunk, (*it)->getLength()); 
			MemChunk * newBlock = new MemChunk(newChunk, newChunkLen);

			// Set the length, push it to Allocated..
			// (*it)->setLength(length);
			temp->setLength(length);
			// allocated.push_back( (*it) );
			allocated.push_back( temp );

			available.erase(it);
			available.push_back(newBlock);
		}
		else{
			temp->setLength(length);
			allocated.push_back( temp );
			available.erase(it);
		}
		MachineResumeSignals(&sigstate);

		mergeMemory();
		checkWaiting();
		return temp->getChunkBase();
	}

	// Purpose: To merge adjacent blocks of free memory
	// Note: I have no idea if this shit works!
	void mergeMemory(){
		TMachineSignalState sigstate;
		MachineSuspendSignals(&sigstate);

		// Merge memory blocks
		for(vector<MemChunk* >::iterator it = available.begin(); it != available.end(); ){
			MemChunk* temp = *it;
			void * addr = static_cast<uint8_t*>((*it)->getChunkBase()) + (*it)->getLength();
			for(vector<MemChunk*>::iterator it2 = ++it; it2 != available.end(); it2++){

				if( !((*it2)->getMerged()) ){
					if( (*it2)->getChunkBase() == addr){
						(*it2)->setMerged(true);
						temp->setLength( temp->getLength() + (*it2)->getLength() );
						it--;
					}//If they're adjacent, mark to merge				
				}//If this memory location has not been merged yet

			}
		}

		// Delete all merged pointers
		for(vector<MemChunk*>::iterator it = available.begin(); it != available.end(); ){
			if( (*it)->getMerged() )
				it = available.erase(it);
			else
				it++;
		}

		MachineResumeSignals(&sigstate);
	}

	void addWaiting( TCB* tcb ){
		if(tcb)
			waitList.push(tcb);
	}

	void checkWaiting(){
		TMachineSignalState sigstate;
		MachineSuspendSignals(&sigstate);

		//Do nothing if its empty
		if(waitList.empty());
		else{

			TCB * next;

			while(  (next = waitList.top() ) && (next) && (next->getState() == VM_THREAD_STATE_DEAD)  ){
				if( waitList.empty() )
					break;
				waitList.pop();
			}
			if( !waitList.empty() )
				waitList.pop();

			if( next && (next->getState() != VM_THREAD_STATE_DEAD) ){

				for(vector<MemChunk*>::iterator it = available.begin(); it != available.end(); it++){
					if( next->getreqLength() <= (*it)->getLength() ){
						next->setState(VM_THREAD_STATE_READY);
						next->setCycles(0);
						next->setSleeping(false);
						rdyQ.push(next);
						break;
					}
				}
			}
		}

		
		MachineResumeSignals(&sigstate);
	}

	bool deallocateMemory(void * base){
		TMachineSignalState sigstate;
		MachineSuspendSignals(&sigstate);

		for(vector<MemChunk*>::iterator it = allocated.begin(); it != allocated.end(); it++){
			if( (*it)->getChunkBase() == base){
				available.push_back( *it );
				allocated.erase(it);
				mergeMemory();
				checkWaiting();
				MachineResumeSignals(&sigstate);
				return true;
			}
		}

		MachineResumeSignals(&sigstate);
		return false;
	}

	bool allocatedEmpty(){
		TMachineSignalState sigstate;
		MachineSuspendSignals(&sigstate);

		if( allocated.empty() ){
			MachineResumeSignals(&sigstate);
			return true;
		}
		MachineResumeSignals(&sigstate);
		return false;
	}

	TVMMemorySize spaceLeft(){
		TMachineSignalState sigstate;
		MachineSuspendSignals(&sigstate);

		TVMMemorySize size = 0;

		for(vector<MemChunk*>::iterator it = available.begin(); it != available.end(); it++)
			size += (*it)->getLength();

		MachineResumeSignals(&sigstate);
		return size;
	}

	void * getBase(){ return baseptr; }

private:
	vector< MemChunk* > available;
	vector< MemChunk* > allocated;
	priority_queue< TCB *, vector<TCB *>, ComparePriority > waitList; //threads waiting for aquisition of the mutex


	TVMMemoryPoolID memId;
	void * baseptr;
	TVMMemorySize length;
};

// Stack of memory pools
vector< MemPool * > memStack;



TVMStatus VMMemoryPoolCreate(void *base, TVMMemorySize size, TVMMemoryPoolIDRef memory){

	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	
	if( (size == 0) || ( base == NULL) || (memory == NULL) ){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	// Set memory ID.
	*memory = memids;

	// Create a new memory pool, push onto vector.
	MemPool* newMem = new MemPool(base, size);
	memStack.push_back(newMem);

	MachineResumeSignals(&sigstate);

	return VM_STATUS_SUCCESS;

}

TVMStatus VMMemoryPoolDelete(TVMMemoryPoolID memory) {
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	for(vector<MemPool*>::iterator it = memStack.begin(); it != memStack.end(); it++){
		if( (*it)->getMemID() == memory ){
			if( (*it)->allocatedEmpty() ){
				memStack.erase(it);
				MachineResumeSignals(&sigstate);
				return VM_STATUS_SUCCESS;
			}
			else
				MachineResumeSignals(&sigstate);
				return VM_STATUS_ERROR_INVALID_STATE;
		}
	}
	
	MachineResumeSignals(&sigstate);
	return VM_STATUS_ERROR_INVALID_PARAMETER;
}

TVMStatus VMMemoryPoolQuery(TVMMemoryPoolID memory, TVMMemorySizeRef bytesleft) {
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);


	if(!bytesleft){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	for(vector<MemPool *>::iterator it = memStack.begin(); it != memStack.end(); it++){
		if( (*it)->getMemID() == memory ){
			*bytesleft = (*it)->spaceLeft();
			MachineResumeSignals(&sigstate);
			return VM_STATUS_SUCCESS;
		}
	}

	MachineResumeSignals(&sigstate);
	return VM_STATUS_ERROR_INVALID_PARAMETER;
}

TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void **pointer) {
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	if( (size == 0) || (!pointer) ){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	for(vector<MemPool*>::iterator it = memStack.begin(); it != memStack.end(); it++){
		if( (*it)->getMemID() == memory ){
			void* tmp = (*it)->firstAvail(size);

			if(!tmp){
				MachineResumeSignals(&sigstate);
				return VM_STATUS_ERROR_INSUFFICIENT_RESOURCES;
			}
			else
				*pointer = tmp;
				MachineResumeSignals(&sigstate);
				return VM_STATUS_SUCCESS;
		}
	}

	MachineResumeSignals(&sigstate);
	return VM_STATUS_ERROR_INVALID_PARAMETER;
}

TVMStatus VMMemoryPoolDeallocate(TVMMemoryPoolID memory, void *pointer) {
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	if(!pointer){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	for(vector<MemPool*>::iterator it = memStack.begin(); it != memStack.end(); it++ ){
		if( (*it)->getMemID() == memory ){
			if( (*it)->deallocateMemory(pointer) ){
				MachineResumeSignals(&sigstate);
				return VM_STATUS_SUCCESS;
			}
			else
				MachineResumeSignals(&sigstate);
				return VM_STATUS_ERROR_INVALID_PARAMETER;
		}
	}
	MachineResumeSignals(&sigstate);
	return VM_STATUS_ERROR_INVALID_PARAMETER;

}


// To be called to schedule the next thread
void VMSchedule(){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	// cout << "Current thread: " << ctcb->getTID() << " will be sleeping for: " << ctcb->getCycles() << " ticks " << endl;
	// cout << "Starting ready queue size: " << rdyQ.size() << endl;

	TCB* next;


	if(rdyQ.empty() && (ctcb->getCycles() == 0) && (!ctcb->getSleeping()) ){
		// cerr << "Decision 1" << endl;
		next = ctcb;
	} // No other threads & current thread is ready to run
	else if(rdyQ.empty()){
		// cerr << "Decision 2" << endl;
		next = tcbs[1];
	} // No other threads, current thread not ready to run
	else{
		// cerr << "Decision 3" << endl;
		while( (next = rdyQ.top()) && (next->getState() == VM_THREAD_STATE_DEAD) ){
			if(rdyQ.empty()){
				next = tcbs[1];
				break;
			}

			rdyQ.pop();
			
		}
		if( next != tcbs[1])
			rdyQ.pop();
		// cerr << "Or not" << endl;
	} // Other threads are ready to run

	// cout << "Switching to thread ID: " << next->getTID() << " from: " << ctcb->getTID() << endl;
	// cout << "Remaining ready queue size: " << rdyQ.size() << endl;


	for( vector<TCB*>::iterator it = tcbs.begin(); it != tcbs.end(); it++){
		if( (*it)->getTID() == ctcb->getTID() ){

			// If the next thread isn't the same one that is currently running
			if((next != ctcb) ){
				// And if the current thread is also not the idle thread / a now dead thread
				if( (ctcb != tcbs[1]) && (ctcb->getState() != VM_THREAD_STATE_DEAD) ){
					// cout << "Pushing thread ID: " << ctcb->getTID() << " onto the waiting Q" << endl;
					ctcb->setState(VM_THREAD_STATE_WAITING);
					waiting.push_back(ctcb);
					// (*it)->setState(VM_THREAD_STATE_WAITING);
					// waiting.push_back( (*it) );
				}
				ctcb = next;
				ctcb->setState(VM_THREAD_STATE_RUNNING);
				// cout << "Current tcb is now: " << ctcb->getTID() << " with old tcb: " << (*it)->getTID() << endl;
				// cout << "With waiting: " << waiting.size() << " and total thread: " << tcbs.size() << endl;
			}
			

			MachineContextSwitch( (*it)->getMCR(), ctcb->getMCR());	
			MachineResumeSignals(&sigstate);
			
			break;
		}
	}
}

// Skeleton entry function: 
void skeletonEntry( void * params ){
	// Get thread entry and params, run it
	MachineEnableSignals();
	TVMThreadEntry e = ctcb->getEntry();
	void * p = ctcb->getParam();
	e(p);

	// cerr << "Terminating from skeletonEntry" << endl;

	// If completed, terminate the thread
	VMThreadTerminate(ctcb->getTID());
	VMSchedule();
}

// For all other file operations, except maybe close.
void MyMachineFileCallback(void * calldata, int result){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);



	// Cast void* to int*
	// Calldata is the thread ID.
	unsigned int * cdata = static_cast<unsigned int*>(calldata);


	// Find the waiting TCB and take it out of the queue
	for(vector<TCB *>::iterator it = waiting.begin(); it != waiting.end(); it++ ){
		// if( ( (*it)->getTID() == *cdata ) && ((*it)->getCycles() == -1) ){
		if( ( (*it)->getTID() == *cdata ) ){
			(*it)->setResult( result + (*it)->getResult() );
			(*it)->setCycles(0);
			(*it)->setSleeping(false);
			(*it)->setState(VM_THREAD_STATE_READY);
			// cout << "Thread ID: " << (*it)->getTID() << " completed IO, now rdy" << endl;
			rdyQ.push( (*it) );
			waiting.erase( it );
			break;
		}
	}

	// If a new thread becomes available that has higher priority
	// than the current running thread, then switch.
	if( rdyQ.top()->getPriority() > ctcb->getPriority() ){
		if( (ctcb->getCycles() == 0) && (ctcb != tcbs[1]) ){
			ctcb->setState(VM_THREAD_STATE_READY);
			rdyQ.push(ctcb);			
		}
		VMSchedule();
	}

	MachineResumeSignals(&sigstate);
}

void MyMachineCloseFileCallback(void * calldata, int result){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	// Cast void* to int*
	// Calldata is the thread ID.
	unsigned int * cdata = static_cast<unsigned int*>(calldata);

	// Find the waiting TCB and take it out of the queue
	for(vector<TCB *>::iterator it = waiting.begin(); it != waiting.end(); it++ ){
		if( (*it)->getTID() == *cdata ){
			waiting.erase( it );
			break;
		}
	}

	// Might be able to merge these two. idk. Will try later. I'm tired rn.

	// Find the tcb, add it to the ready queue.
	for(vector<TCB *>::iterator it = tcbs.begin(); it != tcbs.end(); it++){
		// If specified TCB is found.. add then set it to ready...
		if( (*it)->getTID() == *cdata ){
			if(result > 0){
				for( vector<int>::iterator it = files.begin(); it != files.end(); it++){
					if( *it == result ){
						files.erase(it);
						break;
					}
				}
				// (*it)->deleteFile(result);
			}
			(*it)->setResult(result);
			(*it)->setState(VM_THREAD_STATE_READY);
			(*it)->setCycles(0);
			(*it)->setSleeping(false);
			rdyQ.push( (*it) );
			break;
		}
	}

	if( rdyQ.top()->getPriority() > ctcb->getPriority() ){
		if( (ctcb->getCycles() == 0) && (!ctcb->getSleeping()) ){
			ctcb->setState(VM_THREAD_STATE_READY);
			rdyQ.push(ctcb);			
		}
		VMSchedule();
	}

	MachineResumeSignals(&sigstate);
}

void MyMachineOpenFileCallback(void * calldata, int result){
	// Calldata: 	Indicative of a machine context
	// 				All calldata will be machine context tid
	// 				For now search for tid, but it may be that threads are never taken out of the 
	// 				total or complete job vector. If this is the case, we can directly
	// 				access the context with tcbs[tid]

	// When the callback function returns, it will need to:
	// 1. Set the waiting thread into the ready state
	// 2. Put the thread into the ready queue
	// 3. Provide the return information to the thread (probably want to store this in the TCB)
	// 4. Schedule if necessary

	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	// Calldata is the thread ID.
	unsigned int * cdata = static_cast<unsigned int*>(calldata);

	// Find the waiting TCB and take it out of the queue
	for(vector<TCB *>::iterator it = waiting.begin(); it != waiting.end(); it++ ){
		if( (*it)->getTID() == *cdata ){
			waiting.erase( it );
			break;
		}
	}

	// Might be able to merge these two. idk. Will try later. I'm tired rn.

	// Find the tcb, add it to the ready queue.
	for(vector<TCB *>::iterator it = tcbs.begin(); it != tcbs.end(); it++){
		// If specified TCB is found.. add then set it to ready...
		if( (*it)->getTID() == *cdata ){
			// (*it)->addFile(result);
			// files.push_back(result);
			(*it)->setResult(result);
			(*it)->setState(VM_THREAD_STATE_READY);
			(*it)->setSleeping(false);
			(*it)->setCycles(0);
			rdyQ.push( (*it) );
			break;
		}
	}

	if( rdyQ.top()->getPriority() > ctcb->getPriority() ){
		if( (ctcb->getCycles() == 0) && (!ctcb->getSleeping()) ){
			ctcb->setState(VM_THREAD_STATE_READY);
			rdyQ.push(ctcb);
		}
		VMSchedule();
	}
	MachineResumeSignals(&sigstate);
}

void MyMachineAlarmCallback(void * calldata){
	// Calldata is NOT needed for alarm callback
	// We will instead search through all waiting threads and decrement their sleep counter
	// -1 will indicate blocked until completion (check if a flag has been set)

	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	tickcount++;

	// Pretty sure you can always just push it on...
	if(  ctcb != tcbs[1] && (ctcb->getState() != VM_THREAD_STATE_DEAD) ){
		ctcb->setState(VM_THREAD_STATE_READY);
		rdyQ.push(ctcb);		
	}

	for( vector<TCB *>::iterator it = waiting.begin(); it != waiting.end(); it++){
		if( ((*it)->getCycles() == 1) && ( (*it)->getState() != VM_THREAD_STATE_DEAD ) && ( !(*it)->getSleeping() ) ){
			(*it)->decCycles();
			(*it)->setSleeping(false);
			(*it)->setState(VM_THREAD_STATE_READY);
			rdyQ.push((*it));
			// cout << "QSIZE: " << rdyQ.size() << endl;
		}//Decrement, change state, place into rdyQ
		else if( ((*it)->getCycles() > 1) && ( (*it)->getState() != VM_THREAD_STATE_DEAD ) ){
			(*it)->decCycles();
			// continue;
		}
		else;
			// continue;

		// cout << "Ticking thread ID: " << (*it)->getTID() << " " << (*it)->getCycles() << " ticks remain" << endl;
	} 

	for(vector< TCB*>::iterator it = waiting.begin(); it != waiting.end(); )
	{
		if( ( ((*it)->getCycles() == 0) && (!(*it)->getSleeping())) || ((*it)->getState() == VM_THREAD_STATE_DEAD) )
			it = waiting.erase(it);
		else
			++it;
	}

	// cout << "ReadyQ size: " << rdyQ.size() << endl;

	VMSchedule();
	MachineResumeSignals(&sigstate);
}

void idle(void * p){
	MachineEnableSignals();

	// Wait for callback
	while(1);
}

void prepMemory( void* sharedmem, TVMMemorySize sharedsize, TVMMemorySize heapsize ){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	MemPool * temp = new MemPool( sharedmem, sharedsize);
	memStack.push_back(temp);

	void * heapPtr = malloc(heapsize);
	MemPool* tmp = new MemPool(heapPtr, heapsize);
	memStack.push_back(tmp);

	MachineResumeSignals(&sigstate);
}

bool inSM( void * ptr ){
	if( SMStart <= ptr )
		if( (static_cast<char*>(SMStart) + SMSize) >= ptr )
			return true;

	return false;
}

// TAKEN FROM HERE!!!!!!!: TUAN
// http://stackoverflow.com/questions/27710117/remove-spaces-from-char-array-by-using-a-function-with-a-pointer-argument
void removeSpaces(char* s)
{
    char* cpy = s;  // an alias to iterate through s without moving s
    char* temp = s;

    while (*cpy)
    {
        if (*cpy != ' ')
            *temp++ = toupper(*cpy);
        cpy++;
    }
    *temp = 0;

}

const char* format83(const char* s){
	string temp = s;
	string build;

	int dot = temp.find('.', 0);
	string name = temp.substr(0, dot);

	for( unsigned int i = 0; i < name.length(); i++)
		name[i] = toupper(name[i]);
	while(name.length() < 8)
		name += ' ';

	string ext = temp.substr(dot+1, 3);
	for(unsigned int i = 0; i < ext.length(); i++)
		ext[i] = toupper(ext[i]);
	while(ext.length() < 3)
		ext += ' ';

	build = name + ext;
	return build.c_str();
}

// Offset before it was passed into this function should be in BYTES, not SECTORS. 
TVMStatus readSector(unsigned int offset, void* data){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	int cdata = ctcb->getTID();
	void *calldata = &cdata;
	void (*TMachineFileCallback)(void* data, int result) = MyMachineFileCallback;


	VMMutexAcquire(SECTOR_MUTEX, VM_TIMEOUT_INFINITE);

	MachineFileSeek(FAT_IMAGE_FILE_DESCRIPTOR, offset, 0, TMachineFileCallback, calldata  );

	ctcb->setResult(0);
	ctcb->setCycles(0);
	ctcb->setSleeping(true);
	VMSchedule();

	// Quit here if not expected seek

	MachineFileRead(FAT_IMAGE_FILE_DESCRIPTOR, SHARED_MEM_SECTOR, 512, TMachineFileCallback, calldata );

	ctcb->setResult(0);
	ctcb->setCycles(0);
	ctcb->setSleeping(true);
	VMSchedule();

	// Quit here if read failed

	memcpy(data, SHARED_MEM_SECTOR, 512);
	VMMutexRelease(SECTOR_MUTEX);

	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

// Offset to seek to, data that you want to write out
TVMStatus writeSector(unsigned int offset, void* data){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	int cdata = ctcb->getTID();
	void *calldata = &cdata;
	void (*TMachineFileCallback)(void* data, int result) = MyMachineFileCallback;

	VMMutexAcquire(SECTOR_MUTEX, VM_TIMEOUT_INFINITE);

	MachineFileSeek(FAT_IMAGE_FILE_DESCRIPTOR, offset, 0, TMachineFileCallback, calldata  );

	ctcb->setResult(0);
	ctcb->setCycles(0);
	ctcb->setSleeping(true);
	VMSchedule();

	// Quit here if seek fails, resume signals & release mutex too

	memcpy(SHARED_MEM_SECTOR, data, 512);
	MachineFileWrite(FAT_IMAGE_FILE_DESCRIPTOR, SHARED_MEM_SECTOR, 512, TMachineFileCallback, calldata );

	ctcb->setResult(0);
	ctcb->setCycles(0);
	ctcb->setSleeping(true);
	VMSchedule();

	// Quit here if write fails, resume signals & release mutex too
	VMMutexRelease(SECTOR_MUTEX);

	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

// Read in all clusters given the first cluster
TVMStatus findAll(int firstCluster, uint32_t size, vector<Cluster*> *clusters ){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	int numclus = (size < 1024) ? 1 : (size/1024) + 1;
	int index = firstCluster;
	Cluster* tempClus = new Cluster(index);

	for(int i = 0; i < numclus; i++){
		// Cluster* tempClus = new Cluster(index);
		readSector( (BPBSector.FirstDataSector * 512) + ((index-2) * 1024), tempClus->data );
		readSector( (BPBSector.FirstDataSector * 512) + ((index-2) * 1024 ) + 512, tempClus->data + 512 );
		clusters->push_back(tempClus);
		index = FAT[index];
		tempClus = new Cluster(index);
	}

	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

// Jumps jump number of times through sector clusters if possible, otherwise, return failure
// Passed index is the starting index of FAT
TVMStatus findCluster(uint16_t index, int jumps, Cluster* dataCluster ){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	for(int i = 0; i < jumps; i++){

		if( !(FAT[index] < 0xFFF8) && !( (i+1) < jumps) ){
			MachineResumeSignals(&sigstate);
			return VM_STATUS_FAILURE;
		}
		else if( (FAT[index] < 0xFFF8) && ((i+1) == jumps) ){
			readSector( BPBSector.FirstDataSector * 512 + index * 1024, dataCluster->data );
			readSector( BPBSector.FirstDataSector * 512 + index * 1024 + 512, dataCluster->data + 512 );
			MachineResumeSignals(&sigstate);
			return VM_STATUS_SUCCESS;
		}

		index = FAT[index];
	}

	readSector( BPBSector.FirstDataSector * 512 + index * 1024, dataCluster->data );
	readSector( BPBSector.FirstDataSector * 512 + index * 1024 + 512, dataCluster->data + 512 );

	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus indexCluster(uint16_t index, Cluster* dataCluster){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	dataCluster->dirty =  false;
	readSector( BPBSector.FirstDataSector * 512 + index * 1024, dataCluster->data );
	readSector( BPBSector.FirstDataSector * 512 + index * 1024 + 512, dataCluster->data + 512 );

	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus freeCluster(uint16_t* FATIndex, Cluster* dataCluster){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	int index = -1;

	for(int i = 2; i < BPBSector.ClusterCount; i++){
		if(FAT[i] == 0x0000){
			FAT[i] = 0XFFF8;
			dataCluster->index = *FATIndex = i;
			index = i;
			break;
		}
	}
	if(index == -1){ //Fail to find free cluster..
		MachineResumeSignals(&sigstate);
		return VM_STATUS_FAILURE;
	}
	else{
		readSector( BPBSector.FirstDataSector * 512 + (index - 2) * 1024, dataCluster->data );
		readSector( BPBSector.FirstDataSector * 512 + (index - 2) * 1024 + 512, dataCluster->data + 512 );
	}

	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus readFATROOT(){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	FAT = static_cast<uint16_t*>(malloc(BPBSector.FATSz16 * 512));
	unsigned int FATOffset = BPBSector.Rsvd_SecCnt * 512;
	void* buffer = malloc(512);
	uint16_t * FATbuffer = (uint16_t*)buffer;

	for(int i = 0; i < BPBSector.FATSz16; i++){
		readSector( FATOffset + (i*512), buffer);
		// Its a vector of 16 bit ints (2 bytes each), so 256 iterations
		for(int j = 0; j < 256; j++){
			FAT[i*256+j] = FATbuffer[j];
		}
	}

	// Print FAT array;
	// int j = 0;
	// while(FAT[j] != 0x0){
	// 	cout << hex << FAT[j] << " ";
	// 	j++;
	// }

	unsigned int rootOffset = BPBSector.FirstRootSector * 512;
	uint8_t* rootBuffer = (uint8_t*)buffer;

	Directory* rootDir = new Directory();
	dirSys.push_back(rootDir);
	currentDirectory = rootDir;

	for(int i = 0; i < (BPBSector.RootEntCnt * 32)/512; i++){
		readSector( rootOffset + (i*512), buffer);
		for(int j = 0; j < 512; j++ ){
			ROOT.push_back(rootBuffer[j]);
		}
	}

	// Print ROOT NAMES
	// char temp[12];
	// for(int i = 0; i < (BPBSector.RootEntCnt * 32); i += 32){
	// 	if( (ROOT[i] == 0xE5) || (ROOT[i] == 0x0 ))
	// 		continue;
	// 	if(ROOT[i+11] == (0X01 | 0X02 | 0X04 | 0X08) )
	// 		continue;
	// 	memcpy( (uint8_t*)temp, &(ROOT[i]), 11);
	// 	temp[11] = '\0';
	// 	cout << "SHORT FILE NAME:" << temp << endl;
	// }


	free(buffer);
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus mountFAT(const char* mount){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	int cdata = ctcb->getTID();
	void *calldata = &cdata;
	void (*TMachineFileCallback)(void* data, int result) = MyMachineOpenFileCallback;
	void (*TMachineFileReadCallback)(void* data, int result) = MyMachineFileCallback;

	// Mount the FAT Image
	MachineFileOpen(mount, O_RDWR, 0600, TMachineFileCallback, calldata );

	ctcb->setResult(0);
	ctcb->setCycles(0);
	ctcb->setSleeping(true);
	VMSchedule();

	FAT_IMAGE_FILE_DESCRIPTOR = ctcb->getResult();

	// Create the Mutex reserved for READING/WRITING FAT File System to shared memory
	VMMutexCreate(&SECTOR_MUTEX);
	// Create Mutex reserved for READING/WRITING FROM USER IMPLEMENTED FILE SYSTEM;
	VMMutexCreate(&FILE_SYSTEM_MUTEX);

	// Return if FAT image failed to load
	if(FAT_IMAGE_FILE_DESCRIPTOR < 0){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_FAILURE;
	}

	// Shared Memory Pool allocation to read out ALL FUTURE SECTORS
	// Initialization of SHARED_MEM_SECTOR IS HERE. 
	VMMemoryPoolAllocate(0, 512, &SHARED_MEM_SECTOR);
	uint8_t* BPBSec = static_cast<uint8_t*>(SHARED_MEM_SECTOR);

	MachineFileRead(FAT_IMAGE_FILE_DESCRIPTOR, SHARED_MEM_SECTOR, 512, TMachineFileReadCallback, calldata);

	ctcb->setResult(0);
	ctcb->setCycles(0);
	ctcb->setSleeping(true);
	VMSchedule();

	// Read has succeeded
	if(ctcb->getResult() < 0 ){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_FAILURE;
	}
	else{

		// BPBSector is a global var, BPBSec is a local
		BPBSector.SecPerClus = *(uint8_t*)(BPBSec+13);
		BPBSector.NumFATs = *(uint8_t*)(BPBSec+16);
		BPBSector.Rsvd_SecCnt = *(uint16_t*)(BPBSec+14);
		BPBSector.RootEntCnt = *(uint16_t*)(BPBSec+17);
		BPBSector.TotSec16 = *(uint16_t*)(BPBSec+19);
		BPBSector.FATSz16 = *(uint16_t*)(BPBSec+22);
		BPBSector.TotSec32 = *(uint32_t*)(BPBSec+32);

		BPBSector.FirstRootSector = BPBSector.Rsvd_SecCnt + BPBSector.NumFATs * BPBSector.FATSz16;
		BPBSector.RootDirectorySectors = (BPBSector.RootEntCnt * 32)/512;
		BPBSector.FirstDataSector =  BPBSector.FirstRootSector + BPBSector.RootDirectorySectors;

		if( BPBSector.TotSec16 == 0 )
			BPBSector.ClusterCount =  (BPBSector.TotSec32 - BPBSector.FirstDataSector) / BPBSector.SecPerClus;
		else
			BPBSector.ClusterCount = (BPBSector.TotSec16 - BPBSector.FirstDataSector) / BPBSector.SecPerClus;

	// 	cerr << "Sector per cluster: " << (int)BPBSector.SecPerClus << endl;
	// 	cerr << "Num FATs: " << (int)BPBSector.NumFATs << endl;
	// 	cerr << "Reserved Sector Count: " << BPBSector.Rsvd_SecCnt << endl;
	// 	cerr << "Root Ent Count: " << BPBSector.RootEntCnt << endl;
	// 	cerr << "Total Sector 16: " << BPBSector.TotSec16 << endl;
	// 	cerr << "FAT Size 16: " << BPBSector.FATSz16 << endl;
	// 	cerr << "Total Sector 32: " << BPBSector.TotSec32 << endl;
	// 	cerr << "First Root Sector: " << BPBSector.FirstRootSector << endl;
	// 	cerr << "Root Directory Sector: " << BPBSector.RootDirectorySectors << endl;
		// cerr << "First Data Sector: " << BPBSector.FirstDataSector << endl;
	// 	cerr << "Cluster Count: " << BPBSector.ClusterCount << endl;
	// 	cerr << "End of FAT contents" << endl;
	}

	readFATROOT();
	
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

uint16_t reformatDate( unsigned int DYear, unsigned char DMonth, unsigned char DDay ){
	uint16_t yearBuffer = 0x00, monthBuffer = 0x00, dayBuffer = 0x00;

	yearBuffer = (DYear - 1980) << 9;
	monthBuffer = DMonth << 5;
	dayBuffer = DDay;

	return yearBuffer | monthBuffer | dayBuffer;
}

uint16_t reformatTime( unsigned char DHour, unsigned char DMinute, unsigned char DSecond, unsigned char DHundreth){
	uint16_t hourBuffer = 0x00, minuteBuffer = 0x00, secondBuffer = 0x00;

	hourBuffer = (DHour & 0x1F) << 11;
	minuteBuffer = (DMinute & 0x3F) << 5;
	secondBuffer = (DSecond & 0x1F);

	return hourBuffer | minuteBuffer | secondBuffer;
}

TVMStatus unmountFAT(){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	cerr << "Unmounting the FAT" << endl;

	unsigned int FATOffset = BPBSector.Rsvd_SecCnt * 512;

	// Write out FAT;
	for(int i = 0; i < BPBSector.FATSz16; i++){
		writeSector( FATOffset + (i*512), &FAT[i*256]);
	}

	for(unsigned int i = 0; i < fileSys.size(); i++){
		// Write out Data Cluster
		for(vector<Cluster*>::iterator it = fileSys[i]->clusters.begin(); it != fileSys[i]->clusters.end(); it++){
			writeSector( (BPBSector.FirstDataSector * 512) + (((*it)->index-2) * 1024), (*it)->data);
			writeSector( (BPBSector.FirstDataSector * 512) + (((*it)->index-2) * 1024) + 512, (*it)->data + 512);			
		}

		// Modify Root 
		uint8_t buffer[32];
		uint8_t zero = 0x0;

		// Temporary File copy, change to 8.3 format (implied '.')
		File* file = fileSys[i];
		char* fileName = new char[11];
		fileName = (char*)format83(file->info.DShortFileName);

		// Time formatting 
		uint16_t dCreateDate = reformatDate(file->info.DCreate.DYear, file->info.DCreate.DMonth, file->info.DCreate.DDay);
		uint16_t dCreateTime = reformatTime(file->info.DCreate.DHour, file->info.DCreate.DMinute, file->info.DCreate.DSecond, file->info.DCreate.DHundredth);
		uint16_t dModifyDate = reformatDate(file->info.DModify.DYear, file->info.DModify.DMonth, file->info.DModify.DDay);
		uint16_t dModifyTime = reformatTime(file->info.DModify.DHour, file->info.DModify.DMinute, file->info.DModify.DSecond, file->info.DModify.DHundredth);
		uint16_t dAccessDate = reformatDate(file->info.DAccess.DYear, file->info.DModify.DMonth, file->info.DModify.DDay);

		// ShortNameCopy
		memcpy( buffer, fileName, 11);
		// Attributes Copy
		memcpy( buffer+11, &(file->info.DAttributes), 1);
		// DIR_NTRes
		memcpy( buffer+12, &zero, 1);
		// DIR_CrtTimeTenth
		memcpy( buffer+13, &zero, 1);
		// Create Time & Date
		memcpy( buffer+14, &dCreateTime, 2);
		memcpy( buffer+16, &dCreateDate, 2);
		// Access Date
		memcpy( buffer+18, &dAccessDate, 2);
		// FstClusHI ( Zero'd in FAT16)
		memcpy( buffer+20, &zero, 1);
		memcpy( buffer+21, &zero, 1);
		// Write Time & Date
		memcpy( buffer+22, &dModifyTime, 2);
		memcpy( buffer+24, &dModifyDate, 2);
		// First Cluster Index
		memcpy( buffer+26, &(file->FstClusLO), 2);
		// FileSize
		memcpy( buffer+28, &(file->info.DSize), 4);

		// Copy changes to ROOT
		memcpy( &(ROOT[file->rootOffset]), buffer, 32);
	}

	// I don't know if we have to write out the directory-- we only support ROOT, so probably not necessary.
	// for(unsigned int i = 0; i < dirSys.size(); i++ ){
	// }
	unsigned int rootOffset = BPBSector.FirstRootSector * 512;

	// Write out ROOT
	for(int i = 0; i < (BPBSector.RootEntCnt * 32)/512; i++){
		writeSector( rootOffset + (i*512), &(ROOT[i*512]) );
	}

	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

// Reminder to deallocate the shared mem dedicated for reading sectors later
TVMStatus VMStart(int tickms, TVMMemorySize heapsize, TVMMemorySize sharedsize, const char* mount, int argc, char *argv[]){
	ticksms = tickms;
	usecs = tickms*1000;
	SMSize = sharedsize;


	// Start the machine abstraction layer
	void * sharedmem = MachineInitialize(sharedsize);
	prepMemory(sharedmem, sharedsize, heapsize);

	SMStart = sharedmem;

	TCB* mainThread = new TCB;
 	mainThread->setMC();
 	mainThread->setPriority(VM_THREAD_PRIORITY_NORMAL);
 	mainThread->setState(VM_THREAD_STATE_RUNNING);
 	tcbs.push_back(mainThread);

	// Set current context to Main
	ctcb = tcbs.front();

	// Create Idle Thread, lowest priority.
	TVMThreadID idleID;
 	TVMThreadEntry entry = idle;
	VMThreadCreate(entry, NULL, 0x100000, 0, &idleID);
	// Activate to set machine context, but immediately pop it off the rdyQ.
	VMThreadActivate(idleID);
	rdyQ.pop();

	MachineEnableSignals();


	// Declare callback for machine alarm. No need for calldata.
	void (*TMachineAlarmCallback)(void*) = &MyMachineAlarmCallback;
	MachineRequestAlarm(usecs, TMachineAlarmCallback, NULL);

	mountFAT(mount);

	TVMMainEntry fcn = VMLoadModule( argv[0] );

	if( fcn != NULL ){
		fcn( argc, argv);
		unmountFAT();
		MachineTerminate();
		return VM_STATUS_SUCCESS;
	}
	else
		MachineTerminate();
		return VM_STATUS_FAILURE;
	
}

TVMStatus VMThreadID(TVMThreadIDRef threadref){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	if(threadref == NULL){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	*threadref = ctcb->getTID();

	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid){
	// Entry: Function of the thread
	// Param: Parameters passed to the function
	// memsize: Size of thread's stack
	// prio: priority of thread
	// tid: thread identifier reference

	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	
	if( (entry == NULL) || (tid == NULL) ){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	//Set ThreadID
	*tid = tids;

	// TCB* newThread = new TCB(entry, param, prio, memsize);
	TCB* newThread = new TCB(entry, param, prio);
	TVMStatus stat = newThread->setStack(memsize);
	if(stat != VM_STATUS_SUCCESS){
		delete newThread;
		return VM_STATUS_ERROR_INSUFFICIENT_RESOURCES;
	}
	tcbs.push_back(newThread);

	MachineResumeSignals(&sigstate);

	// cout << "Exit VMThreadCreate" << endl;
	return VM_STATUS_SUCCESS;

}

TVMStatus VMThreadDelete(TVMThreadID thread){

	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	for(vector<TCB*>::iterator it = tcbs.begin(); it != tcbs.end(); it++){
		if( (*it)->getTID() == thread ){
			if((*it)->getState() == VM_THREAD_STATE_DEAD){
				tcbs.erase(it);
				MachineResumeSignals(&sigstate);
				return VM_STATUS_SUCCESS;
			}
			MachineResumeSignals(&sigstate);
			return VM_STATUS_ERROR_INVALID_STATE;
		}
	}
	MachineResumeSignals(&sigstate);
	return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMThreadActivate(TVMThreadID thread){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	// If there is no need to ever DELETE a TCB (rather than making state dead)
	// Then no searching is required, and we can simply index with tid!
	for(vector<TCB *>::iterator it = tcbs.begin(); it != tcbs.end(); it++){
		// Grab the requested thread
		if( (*it)->getTID() == thread ){

			// VMThreadActivate should only activate dead threads
			if( (*it)->getState() != VM_THREAD_STATE_DEAD ){
				MachineResumeSignals(&sigstate);
				return VM_STATUS_ERROR_INVALID_STATE;
			}

			(*it)->setCycles(0);
			(*it)->setSleeping(false);
			(*it)->setState(VM_THREAD_STATE_READY);
			// cout << "RdyQ Size VMActivate Before: " << rdyQ.size() << endl; 
			rdyQ.push((*it));
			// cout << "RdyQ Size VMActivate After: " << rdyQ.size() << endl; 

			TVMThreadEntry se = skeletonEntry;
			// MachineContextCreate( (*it)->getMCR(), (*it)->getEntry(), (*it)->getParam(), (*it)->getStack(), (*it)->getSSize() );
			MachineContextCreate( (*it)->getMCR(), se, (*it)->getParam(), (*it)->getStack(), (*it)->getSSize() );
			// Ready, should be inserted into a queue later...

			if( rdyQ.top()->getPriority() > ctcb->getPriority() ){
				ctcb->setCycles(0);
				ctcb->setSleeping(false);
				ctcb->setState(VM_THREAD_STATE_READY);
				rdyQ.push(ctcb);
				VMSchedule();
			}

			MachineResumeSignals(&sigstate);
			return VM_STATUS_SUCCESS;
		}
	}

	MachineResumeSignals(&sigstate);
	// ID requested does not exist
	return VM_STATUS_ERROR_INVALID_ID;
}

// Thread Mutex releaase is for the specific purpose for releasing mutexes and not scheduling directly afterwards
// Code is similiar to VMMutexRelease otherwise.
TVMStatus VMThreadMutexRelease( TVMMutexID mutex ){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	TCB* next; 

	//might need to add a condition where if waitList is empty, and if acquiredThread is done with the mutex,
	//we set the aquired thread to NULL or something signifying the Mutex no longer needed at the moment. 
	//Then, we can safely delete the mutex.

	for (vector< Mutex* >::iterator itr = mutexes.begin(); itr != mutexes.end(); itr++) {
		//If its the right mutex
		if ((*itr)->getMutexID() == mutex) {

				//If the queue is empty
				if ( (*itr)->checkQueueEmpty() ) { //we might need to check if the mutex is still held by the acquiredThread
					(*itr)->setLock(true);
					(*itr)->setAcquiredThread(NULL);
					MachineResumeSignals(&sigstate);
					return VM_STATUS_SUCCESS; //not sure if this is right return status
				}
				//Else, change owners
				while( (next = (*itr)->getNextThread()) && (next->getCycles() == 0) && (next->getState() != VM_THREAD_STATE_DEAD) && (!next->getSleeping())){
					if( (*itr)->checkQueueEmpty()){
						next = NULL;
						(*itr)->setLock(true);
						MachineResumeSignals(&sigstate);
						return VM_STATUS_SUCCESS;
					}
					(*itr)->DequeueThread();
				}
				(*itr)->DequeueThread();
		
				// Change ownership of mutex
				(*itr)->setAcquiredThread(next);
				// Set newThread State to READY & cycles to 0 before pushing onto Q
				next->setState(VM_THREAD_STATE_READY);
				next->setCycles(0);
				next->setSleeping(false);
				// Push next thread on rdyQ
				rdyQ.push(next);
				MachineResumeSignals(&sigstate);
				return VM_STATUS_SUCCESS; //not sure if this is right but we need to return some status
		}
	} 

	//we need to add a return VM_STATUS_ERROR_INVALID_STATE; somewhere.. not sure where is the best place :(
	//VM_STATUS_ERROR_INVALID_STATE is true when mutex exists but is not currently held by the running thread. 

	MachineResumeSignals(&sigstate);
	return VM_STATUS_ERROR_INVALID_ID;

}

TVMStatus VMThreadTerminate(TVMThreadID thread){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	for(vector<TCB* >::iterator it = tcbs.begin(); it != tcbs.end(); it++){
		if( (*it)->getTID() == thread ){


			if( (*it)->getState() == VM_THREAD_STATE_DEAD ){
				MachineResumeSignals(&sigstate);
				return VM_STATUS_ERROR_INVALID_STATE;
			}

			(*it)->setCycles(10);
			(*it)->setSleeping(true);
			(*it)->setState(VM_THREAD_STATE_DEAD);
			// Search waiting vector to remove... 
			// Search rdyQ to terminate...
			// Searching mutex queue remove...
			for(vector<Mutex *>::iterator it2 = mutexes.begin(); it2 != mutexes.end(); it2++){
				if( !( (*it2)->getAcquiredThread() ) )
					continue;
				if( ((*it)->getTID() == (*it2)->getTID()) ){
					VMThreadMutexRelease( (*it2)->getMutexID() );
				}
			}

			VMSchedule();
			MachineResumeSignals(&sigstate);
			return VM_STATUS_SUCCESS;
		}

	}

	MachineResumeSignals(&sigstate);
	return VM_STATUS_ERROR_INVALID_ID;

}


// TVMStatus VMThreadID(TVMThreadIDRef threadref);
TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef stateref){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	if(stateref == NULL){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	for(vector<TCB *>::iterator it = tcbs.begin(); it != tcbs.end(); it++){
		if( (*it)->getTID() == thread){
			*stateref = (*it)->getState();
			MachineResumeSignals(&sigstate);
			return VM_STATUS_SUCCESS;
		}
	}
	MachineResumeSignals(&sigstate);
	return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	if( !filename || !filedescriptor ){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	VMMutexAcquire(FILE_SYSTEM_MUTEX, VM_TIMEOUT_INFINITE);

	const char* format83filename = format83(filename);


	// cerr << "Called VMFIleOPen"  << endl;
	int fileIndex = 0;
	fileIndex = searchFilename(filename);
	if(fileIndex >= 0){
		// cerr << "Found filename within system" << endl;
	}
	// char fname[13] = {' '}; 
	char fname[12] = {' '};
	int i;
	for(i = 0; i < (BPBSector.RootEntCnt * 32) && (ROOT[i] != 0); i += 32){

		// Skip LFN for now
		if(ROOT[i+11] == 0x0F)
			continue;

		memcpy( (uint8_t*)fname, &(ROOT[i]), 11);
		// memcpy( (uint8_t*)fname, &(ROOT[i]), 8 );
		// memcpy( (uint8_t*)(fname+8), &(ROOT[i+7]), 4);
		// fname[8] = '.';
		// fname[12] = '\0';
		fname[11] = '\0';


		// removeSpaces(fname);
		// cerr << "Format83 style: " << format83filename << endl;
		// cerr << "Found file: " << fname << " comparing to: " << format83filename << endl;

		// if( !(strcmp( fname, filename )) ){
		if( !(strcmp(fname, format83filename)) ){
			// cerr << "Found File in FAT, opening: " << filename << endl;
			File* temp = new File(&ROOT[i], flags, filename, i);
			temp->curCluster = 0;
			temp->offset = 0;
			*filedescriptor = temp->fd;
			fileSys.push_back(temp);
			VMMutexRelease(FILE_SYSTEM_MUTEX);
			MachineResumeSignals(&sigstate);
			return VM_STATUS_SUCCESS;
		}
		else
			continue;
	}
	// If not found IN ROOT, then create a new one...
	if( flags & O_CREAT ){
		// cerr << "Not found, creating.." << endl;
		if( ROOT[i] != 0 )
			while(ROOT[i+=32] != 0);

		//FILE NAME CREATION WHERE ROOT IS 0..
		File* temp = new File(filename, flags, &ROOT[i], i);
		*filedescriptor = temp->fd;
		fileSys.push_back(temp);
		VMMutexRelease(FILE_SYSTEM_MUTEX);
		MachineResumeSignals(&sigstate);
		return VM_STATUS_SUCCESS;
	}


	VMMutexRelease(FILE_SYSTEM_MUTEX);
	MachineResumeSignals(&sigstate);
	if( ctcb->getResult() >= 0 )
		return VM_STATUS_SUCCESS;
	else
		return VM_STATUS_FAILURE;
}

// This one is correct
TVMStatus VMFileClose(int filedescriptor){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	if(filedescriptor < 3){
		// Calldata will be the thread ID
		int cdata = ctcb->getTID();
		void *calldata = &cdata;

		// Declare callback fcn
		void (*TMachineFileCallback)(void* data, int result) = MyMachineCloseFileCallback;

		// Result is set within callback function
		MachineFileClose(filedescriptor, TMachineFileCallback, calldata);

		// Block and sch-edule
		// Changed to waiting in VMSchedule
		ctcb->setCycles(0);
		ctcb->setSleeping(true);
		VMSchedule();
		// The actual removal of the file in the list is in the callback function.

		// Check result set within callback
		MachineResumeSignals(&sigstate);
		if( ctcb->getResult() >= 0 )
			return VM_STATUS_SUCCESS;
		else
			return VM_STATUS_FAILURE;
	}
	else{
		VMMutexAcquire(FILE_SYSTEM_MUTEX, VM_TIMEOUT_INFINITE);

		int index = searchFile(filedescriptor);

		if(index < 0 ){
			VMMutexRelease(FILE_SYSTEM_MUTEX);
			MachineResumeSignals(&sigstate);
			return VM_STATUS_FAILURE;
		}

		File* temp = fileSys[index];
		temp->openCount--;

		VMMutexRelease(FILE_SYSTEM_MUTEX);
		MachineResumeSignals(&sigstate);
		return VM_STATUS_SUCCESS;
	}

}

TVMStatus VMFileRead(int filedescriptor, void *data, int *length){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	if( (data == NULL) || (length == NULL ) ){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	VMMutexAcquire(FILE_SYSTEM_MUTEX, VM_TIMEOUT_INFINITE);

	if(filedescriptor < 3){
		int cdata = ctcb->getTID();
		void *calldata = &cdata;

		ctcb->setResult(0);

		// The 1st member of memStack will always be shared memory.
		// The 2nd will probably be the heap memory pool.
		void * chunk = memStack[0]->firstAvail( *length );


		if(!chunk){
			ctcb->setCycles(0);
			ctcb->setSleeping(true);
			ctcb->setreqLength(  ((*length)+63)/64 * 64  );
			memStack[0]->addWaiting(ctcb);
			VMSchedule();
			chunk = memStack[0]->firstAvail( *length );
		}//Full block until space available

		void (*TMachineFileCallback)(void* data, int result) = MyMachineFileCallback;

		if( *length < 512 ){
			MachineFileRead(filedescriptor, chunk, *length, TMachineFileCallback, calldata);

			// Block and schedule
			ctcb->setCycles(0);
			ctcb->setSleeping(true);
			VMSchedule();
		}
		else{
			int i, temp = *length;
			for(i = 0; i < *length; i+=512){
				MachineFileRead(filedescriptor, static_cast<uint8_t*>(chunk) + i , 512, TMachineFileCallback, calldata);
				ctcb->setCycles(0);
				ctcb->setSleeping(true);
				VMSchedule();
				temp -= 512;
			}

			if( temp != 0){
				i -= 512;
				MachineFileRead(filedescriptor, static_cast<uint8_t*>(chunk) + i , temp, TMachineFileCallback, calldata);			
			}
		}
		memcpy(data, chunk, *length);
		*length = ctcb->getResult();
	}
	else{
		// Check if the file has been opened by this thread
		int index = searchFile(filedescriptor);

		if(index < 0){
			VMMutexRelease(FILE_SYSTEM_MUTEX);
			MachineResumeSignals(&sigstate);
			return VM_STATUS_FAILURE;
		}

		File* temp = fileSys[index];


		SVMDateTime tempDateTime;
		VMDateTime(&tempDateTime);
		temp->info.DAccess = tempDateTime;

		int twoCluster = 0, firstLength = 0, offFile = 0;

		// If the READ goes OFF the file, you must account for this
		// You only read in as much as possibl;e
		// cerr << "FileSize: " << temp->info.DSize;
		// cerr << " Current cluster: " << temp->curCluster << " Current offset: " << temp->offset << " FileRead Req: " << *length << endl;
		offFile = ( ((temp->curCluster) * 1024 + temp->offset + *length) > temp->info.DSize ) ? temp->info.DSize - (temp->curCluster) * 1024 - temp->offset : *length;

		// cerr << "Infinite loop? "<< endl;
		if(offFile == 0){
			// cerr << "Read with no length" << endl;
			*length = 0;
			VMMutexRelease(FILE_SYSTEM_MUTEX);
			MachineResumeSignals(&sigstate);
			return VM_STATUS_FAILURE;
		}

		//Totally redundant code leaving in for now cause tired
		if(offFile == *length) // If not off file, then check if off cluster boundary with normal length
			twoCluster = ( ( temp->offset + *length) >= 1024 ) ? temp->curCluster + 1 : twoCluster;
		else //If you shorten the read amt, then check if you are still off the cluster boundary with the shortened length
			twoCluster = ( ( temp->offset + offFile) >= 1024 ) ? temp->curCluster + 1 : twoCluster;

		// Fucking have to account for reads OVER the length of one cluster (1024), like three.
		if(offFile == *length ){
			if(twoCluster){
				// cerr << "Cluster Start: " << temp->curCluster << " Offset start: " << temp->offset << endl;
				firstLength = 1024 - temp->offset;
				int totalLength = firstLength;
				// cerr << "Total Size: " << temp->clusters.size();
				// cerr << " Double cluster: " <<  temp->curCluster << " offset: " << temp->offset << endl;
				memcpy( data, (temp->clusters[temp->curCluster]->data + temp->offset), firstLength);

				temp->offset = 0;
				temp->curCluster++;


				if( (temp->curCluster + 1) > temp->clusters.size() ){
					// cerr << "Return on one read" << endl;
					*length = totalLength;
					VMMutexRelease(FILE_SYSTEM_MUTEX);
					// cerr << "Mutex released" << endl;
					MachineResumeSignals(&sigstate);
					// cerr << "Returning" << endl;
					return VM_STATUS_SUCCESS;
				}


				int nextLength = 0;
				int remainder = *length - totalLength;

				do{
					if( (remainder = *length - totalLength) > 1024)
						nextLength = 1024;
					else
						nextLength = remainder;
					// cerr << "File descriptor Read: " << temp->fd << endl;
					// cerr << "Next length: " << nextLength << " File size: " << temp->info.DSize;
					// cerr << " Total Clusters: " << temp->clusters.size();
					// cerr << " Current Cluster: " << temp->curCluster << " Offset: " << temp->offset << endl;
					// cerr << "Computed offset: " << (temp->curCluster * 1024) + temp->offset << endl;
					// cerr << "Total lenth: " << totalLength << endl;
					// cerr << "Pair1~" << endl;
					memcpy( (uint8_t*)(data) + totalLength, (temp->clusters[temp->curCluster]->data + temp->offset), nextLength);
					// cerr << "Pair2~" << endl;

					if(nextLength == 1024)
						temp->curCluster++;
					else
						temp->offset += nextLength;

					totalLength += nextLength;

				}while( (*length - totalLength) > 0);

			}//Read more than once
			else{
				// cerr << "Write once "<< endl;
				memcpy(data, (temp->clusters[temp->curCluster]->data + temp->offset), *length);
				temp->offset += *length;
			}//Read Once

		} // Read full length
		else{
			if(twoCluster){
				// cerr << "Not full read" << endl;
				firstLength = 1024 - temp->offset;
				// int totalLength = offFile - firstLength;
				int totalLength = firstLength;
				memcpy( data, (temp->clusters[temp->curCluster]->data + temp->offset), firstLength);
				temp->offset = 0;
				temp->curCluster++;

				int nextLength = 0;
				int remainder = offFile - totalLength;

				do{
					if( (remainder = offFile - totalLength) > 1024)
						nextLength = 1024;
					else
						nextLength = remainder;

					// cerr << "Current cluster: " << temp->curCluster << " offset: " << temp->offset << endl;
					// cerr << "Total File Size: " << temp->info.DSize << " num Clusters: " << temp->clusters.size() << endl;
 
					memcpy( (uint8_t*)(data) + totalLength, (temp->clusters[temp->curCluster]->data + temp->offset), nextLength);


					if(nextLength == 1024)
						temp->curCluster++;
					else
						temp->offset += nextLength;

					totalLength += nextLength;

				}while( (offFile - totalLength) > 0);
				
			}// Read a portion from two clusters
			else{
				// cerr << "Reading from only one????" << endl;
				// cerr << "Current cluster: " << temp->curCluster << " Current offset: " << temp->offset << " FileRead Length: " << offFile << endl;
				memcpy(data, (temp->clusters[temp->curCluster]->data + temp->offset), offFile);
				// cerr << "After one read" << endl;
				temp->offset += offFile;
			}//Read a portion from only one cluster

			*length = offFile;
		}//Read only a portion


	}
	VMMutexRelease(FILE_SYSTEM_MUTEX);
	MachineResumeSignals(&sigstate);
	if( ctcb->getResult() >= 0)
		return VM_STATUS_SUCCESS;
	else
		return VM_STATUS_FAILURE;

}

TVMStatus VMFileWrite(int filedescriptor, void *data, int *length){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	if( (data == NULL) || (length == NULL) ){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}


	if(filedescriptor < 3){
		int cdata = ctcb->getTID();
		void *calldata = &cdata;

		// Declare callback function
		void (*TMachineFileCallback)(void* data, int result) = MyMachineFileCallback;

		// Find first available chunk of memory that meets requirements
		void * chunk = memStack[0]->firstAvail( *length );

		if(!chunk){
			ctcb->setCycles(0);
			ctcb->setSleeping(true);
			ctcb->setreqLength(  ((*length)+63)/64 * 64  );
			memStack[0]->addWaiting(ctcb);
			VMSchedule();
			chunk = memStack[0]->firstAvail( *length );
		}//Full, block until available;

		memcpy(chunk, data, *length);

		// Set the result to 0; in callback functions we will end up adding result value
		ctcb->setResult(0);

	
		if( *length <= 512 ){
			MachineFileWrite(filedescriptor, chunk, *length, TMachineFileCallback, calldata);
			ctcb->setCycles(0);
			ctcb->setSleeping(true);
			VMSchedule();
		}
		else{
			int i, temp = *length;
			for(i = 0; i < *length; i+=512){
				MachineFileWrite(filedescriptor, static_cast<uint8_t*>(chunk) + i , 512, TMachineFileCallback, calldata);
				ctcb->setCycles(0);
				ctcb->setSleeping(true);
				VMSchedule();
				temp -= 512;
			}

			if( temp != 0){
				i -= 512;
				MachineFileWrite(filedescriptor, static_cast<uint8_t*>(chunk) + i , temp, TMachineFileCallback, calldata);			
			}
		}

		VMMemoryPoolDeallocate(0, chunk);

		// cerr << "Finished write" << endl;
		*length = ctcb->getResult();		
	}
	else{ // Crazy FileWrite stuff here
		VMMutexAcquire(FILE_SYSTEM_MUTEX, VM_TIMEOUT_INFINITE);

		int index = searchFile(filedescriptor);
		if(index < 0 ){
			VMMutexRelease(FILE_SYSTEM_MUTEX);
			MachineResumeSignals(&sigstate);
			return VM_STATUS_FAILURE;
		}

		File* temp = fileSys[index];

		// Read ONLY FILE, QUIT
		if( temp->info.DAttributes & 0x01 ){
			VMMutexRelease(FILE_SYSTEM_MUTEX);
			MachineResumeSignals(&sigstate);
			return VM_STATUS_FAILURE;
		}

		int newOffset = temp->offset + *length;
		// Data write runs off of a cluster
		if( newOffset >= 1024 ){

			// Must write once
			int firstWrite = 1024 - temp->offset;
			temp->clusters[temp->curCluster]->dirty = true;
			temp->dirty = true;
			memcpy( (temp->clusters[temp->curCluster]->data + temp->offset), data, firstWrite);
			temp->offset = 0;

			// Set up calculations for the next writes to different clusters
			int nextWrite;
			int totalWrite = firstWrite;
			int remainder = *length - totalWrite;

			// Check if we have a next cluster to write to, if not, make one & move curCluster..
			if( remainder > 0){ 
				do{
					// If we're not going off the cluster, then you can increment the cluster to the next one to write to
					if( (temp->curCluster+1) < temp->clusters.size() ){
						temp->curCluster++;
					}
					// Otherwise, since it's a write, we will add mnore to the end of the file, so find a new cluster
					// and write to it
					else{
						uint16_t FATindex;
						// You pass in 0 as a dummy value, the index (FATOFFSET) will be overwritten inside
						Cluster* tempClus = new Cluster(0);
						freeCluster(&FATindex, tempClus);
						FAT[temp->clusters[temp->curCluster]->index] = FATindex;
						temp->clusters.push_back(tempClus);
						temp->curCluster++;
					}//Else in While

					if( (remainder = *length - totalWrite) > 1024)
						nextWrite = 1024;
					else
						nextWrite = remainder;

					totalWrite += nextWrite;
					// cerr << "Pair 1" << endl;
					memcpy( (temp->clusters[temp->curCluster]->data), (uint8_t*)data+totalWrite, nextWrite );

					// cerr << "Pair 2" << endl;
					if( nextWrite != 1024 )
						temp->offset += nextWrite;
					// Check if it wrote larger than the filesize... then increment size too here....


				} while( (remainder = *length - totalWrite) > 0 );

				if( temp->info.DSize < (( temp->clusters.size() * 1024 ) + temp->offset) )
						temp->info.DSize += totalWrite;

			
			}
			else{
				// cerr << "Even write" << endl;
				uint16_t FATindex;
				Cluster* tempClus = new Cluster(0);
				freeCluster(&FATindex, tempClus);
				FAT[temp->clusters[temp->curCluster]->index] = FATindex;
				temp->clusters.push_back(tempClus);

				temp->curCluster++;
				// cerr << "! New Cluster: " << temp->curCluster << endl;
				if( temp->info.DSize < (( temp->clusters.size() * 1024 ) + temp->offset) )
						temp->info.DSize += *length;

			}

			
			// cerr << "Number of clusters: " << temp->clusters.size(); 
			// cerr << " Current cluster: " << temp->curCluster;
			// cerr << " Current offset: " << temp->offset << endl;
			// Write again
		}
		else{
			// cerr << "Inline write fd: " << temp->fd << endl;
			// cerr << "Before Curcluster: " << temp->curCluster << " offset: " << temp->offset << endl;
			temp->clusters[temp->curCluster]->dirty = true;
			temp->dirty = true;
			memcpy( (temp->clusters[temp->curCluster]->data + temp->offset), data, *length );
			temp->offset = newOffset;

			if( temp->info.DSize < (( temp->clusters.size() * 1024 ) + temp->offset) )
				temp->info.DSize += *length;
			// cerr << "After Curcluster: " << temp->curCluster << " offset: " << temp->offset << endl;
		}

		SVMDateTime tempDateTime;
		VMDateTime(&tempDateTime);
		temp->dirty = true;
		temp->info.DModify = temp->info.DAccess = tempDateTime;


		VMMutexRelease(FILE_SYSTEM_MUTEX);
		MachineResumeSignals(&sigstate);
		return VM_STATUS_SUCCESS;
	}
 
	MachineResumeSignals(&sigstate);
	if( ctcb->getResult() >= 0)
		return VM_STATUS_SUCCESS;
	else
		return VM_STATUS_FAILURE;
}


TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	if( newoffset == NULL ){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	int index = searchFile(filedescriptor);

	if(index < 0){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_FAILURE;
	}

	File* temp = fileSys[index];

	int startCluster = (whence+offset <= 1024 ) ? 0 : whence/1024;
	temp->curCluster = startCluster;	
	int newOffset = whence + offset;
	if( newOffset >= 1024){
		newOffset %= 1024;
		temp->curCluster++;
	}
	temp->offset = newOffset;

	// Change to the new offset... 
	*newoffset = temp->offset;

	// cerr << "Seek Cluster: " << temp->curCluster;
	// cerr << "Seek Offset: " << temp->offset << endl;

	MachineResumeSignals(&sigstate);
	if( ctcb->getResult() >= 0)
		return VM_STATUS_SUCCESS;
	else
		return VM_STATUS_FAILURE;

}

TVMStatus VMThreadSleep(TVMTick tick){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	if(tick == VM_TIMEOUT_INFINITE){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	else if( tick == VM_TIMEOUT_IMMEDIATE ){
		ctcb->setCycles(1);
		ctcb->setSleeping(false);
	}
	else{
		// Current thread will sleep for tick cycles
		// This function should only expect positive cycles..
		ctcb->setCycles(tick);
		ctcb->setSleeping(false);			
	}

	// Block until then
	VMSchedule();

	MachineResumeSignals(&sigstate);

	return VM_STATUS_SUCCESS;
}

TVMStatus VMTickMS(int *tickmsref){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	if(!tickmsref){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	*tickmsref = ticksms;

	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMTickCount(TVMTickRef tickref){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	if(!tickref){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	*tickref = tickcount;

	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;	
}

TVMStatus VMMutexCreate(TVMMutexIDRef mutexref){

	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);
	
	if( mutexref == NULL ){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	*mutexref = mids; //*mutexref orginally *mutex

	Mutex* newMutex = new Mutex();
	mutexes.push_back(newMutex);

	MachineResumeSignals(&sigstate);

	return VM_STATUS_SUCCESS;

}

TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout){

	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);


	// cerr << "Acquired called MID: " << mutex << endl;
	for (vector< Mutex* >::iterator itr = mutexes.begin(); itr != mutexes.end(); itr++) {
		if ( (*itr)->getMutexID() == mutex) { //check if it exists with matching id
			if(  !( (*itr)->getLock() )  ) { //if the lock is aquired by another thread
				if (timeout == VM_TIMEOUT_INFINITE) {
					ctcb->setCycles(0); //tell current tcb to wait indefinitely for mutex
					ctcb->setSleeping(true);
					(*itr)->addToQueue(ctcb);
					VMSchedule(); //schedule new thread
					// (*itr)->setAcquiredThread(ctcb);
					MachineResumeSignals(&sigstate);
					return VM_STATUS_SUCCESS;
				}
				else if( timeout == VM_TIMEOUT_IMMEDIATE){
					MachineResumeSignals(&sigstate);
					return VM_STATUS_FAILURE;
				}
				else //if specific number
					ctcb->setCycles(timeout); //wait for specific length of time
					ctcb->setSleeping(false);
					(*itr)->addToQueue(ctcb);
					VMSchedule(); //schedule new thread
					MachineResumeSignals(&sigstate);
					if ((*itr)->getAcquiredThread()->getTID() == ctcb->getTID())
						return VM_STATUS_SUCCESS;
					return VM_STATUS_FAILURE;
			
			}
			else{
				// In the future make the setAcquiredThread method to
				(*itr)->setAcquiredThread(ctcb);
				(*itr)->setLock(false);
				MachineResumeSignals(&sigstate);
				return VM_STATUS_SUCCESS;
			}
		}
	}

	MachineResumeSignals(&sigstate);
	return VM_STATUS_ERROR_INVALID_ID;

	//mutex->setAcquiredThread();
}


TVMStatus VMMutexRelease(TVMMutexID mutex){

	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	TCB* next; 

	//might need to add a condition where if waitList is empty, and if acquiredThread is done with the mutex,
	//we set the aquired thread to NULL or something signifying the Mutex no longer needed at the moment. 
	//Then, we can safely delete the mutex.

	for (vector< Mutex* >::iterator itr = mutexes.begin(); itr != mutexes.end(); itr++) {
		//If its the right mutex
		if ((*itr)->getMutexID() == mutex) {
			//If we current tcb owns it
			if( (*itr)->getAcquiredThread()->getTID() == ctcb->getTID() ){
				//If the queue is empty
				if ( (*itr)->checkQueueEmpty() ) { //we might need to check if the mutex is still held by the acquiredThread
					(*itr)->setLock(true);
					(*itr)->setAcquiredThread(NULL);
					MachineResumeSignals(&sigstate);
					return VM_STATUS_SUCCESS; //not sure if this is right return status
				}
				//Else, change owners
				while( (next = (*itr)->getNextThread()) && (next->getCycles() == 0) && (next->getState() != VM_THREAD_STATE_DEAD) && (!next->getSleeping())){
					(*itr)->DequeueThread();
				}
				(*itr)->DequeueThread();
				// Change ownership of mutex
				(*itr)->setAcquiredThread(next);
				// Set newThread State to READY & cycles to 0 before pushing onto Q
				next->setState(VM_THREAD_STATE_READY);
				next->setCycles(0);
				next->setSleeping(false);
				// Push next thread on rdyQ
				rdyQ.push(next);
				// (*itr)->addToQueue(next);
				// Peek at top thread on rdyQ to see if priority is higher
				if( rdyQ.top()->getPriority() > ctcb->getPriority() ) {
					// if it is higher, then set current thread to ready, set cycles to 0, push to rdyQ, then call VMSchedule
					ctcb->setState(VM_THREAD_STATE_READY); //not sure if the following code is correct, let me know if my interpretation is wrong
					ctcb->setCycles(0);
					ctcb->setSleeping(false);
					rdyQ.push(ctcb);
					VMSchedule();					
				}
					MachineResumeSignals(&sigstate);
					return VM_STATUS_SUCCESS; //not sure if this is right but we need to return some status
			}
			MachineResumeSignals(&sigstate);
			return VM_STATUS_ERROR_INVALID_STATE;
		}
	} 

	//we need to add a return VM_STATUS_ERROR_INVALID_STATE; somewhere.. not sure where is the best place :(
	//VM_STATUS_ERROR_INVALID_STATE is true when mutex exists but is not currently held by the running thread. 

	MachineResumeSignals(&sigstate);
	return VM_STATUS_ERROR_INVALID_ID;

}


TVMStatus VMMutexDelete(TVMMutexID mutex){

	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);


	//check my work please!
	for (vector< Mutex* >::iterator itr = mutexes.begin(); itr != mutexes.end(); itr++) {
		if ((*itr)->getMutexID() == mutex) { //if mutex found
			if ((*itr)->checkQueueEmpty() ) { //delete mutex if there are no threads waiting 
				(*itr)->setLock(true);					//we might need to check if the mutex is still held by the acquired thread
				mutexes.erase(itr);
				MachineResumeSignals(&sigstate);
				return VM_STATUS_SUCCESS;
			}
			else 
				MachineResumeSignals(&sigstate);
				return VM_STATUS_ERROR_INVALID_STATE; //if mutex exists but is currently held by a thread
		}
	}

	MachineResumeSignals(&sigstate);		
	return VM_STATUS_ERROR_INVALID_ID;

}


TVMStatus VMMutexQuery(TVMMutexID mutex, TVMThreadIDRef ownerref){

	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	//check my work please!
	if( ownerref == NULL ){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	for (vector< Mutex* >::iterator itr = mutexes.begin(); itr != mutexes.end(); itr++) {
		if ((*itr)->getMutexID() == mutex) {
			//according to the project guidelines, we are supposed to find the acquiredthread of the specified
			//mutex and place the acquiredthreads threadID into ownerref.
			if( ((*itr)->getLock()) ){
				*ownerref = VM_THREAD_ID_INVALID;
				MachineResumeSignals(&sigstate);
				return VM_STATUS_SUCCESS;
			}


			*ownerref = (*itr)->getAcquiredThread()->getTID(); 
			MachineResumeSignals(&sigstate);
			 return VM_STATUS_SUCCESS;
		}
		}

	MachineResumeSignals(&sigstate);
	return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMDirectoryOpen(const char* dirname, int *dirdescriptor){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	if( !dirname || !dirdescriptor){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	if( VMFileSystemValidPathName(dirname) == VM_STATUS_ERROR_INVALID_PARAMETER ){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	VMMutexAcquire(FILE_SYSTEM_MUTEX, VM_TIMEOUT_INFINITE);
	
	// cerr << "Dirname: " << dirname << endl;
	// char* simplePath = new char[256];
	char* abspath = new char[256];


	// VMFileSystemSimplifyPath(simplePath, dirSys[0]->pathname.c_str(), dirname );
	// cerr << "Simplepath: " << simplePath << endl;
	VMFileSystemGetAbsolutePath(abspath, dirSys[0]->pathname.c_str(), dirname);
	// cerr << "Abspath: " << abspath	 << endl; 

	int index = searchDirectoryFilename(abspath);
	if(index >= 0 ){
		Directory* temp = dirSys[index];
		*dirdescriptor = temp->dd;
		VMMutexRelease(FILE_SYSTEM_MUTEX);
		MachineResumeSignals(&sigstate);
		return VM_STATUS_SUCCESS;
	}

	const char* format83dirname = format83(dirname);



	for(int i = 0; (i < BPBSector.RootEntCnt * 32) && (ROOT[i] != 0); i += 32 ){

		//Skip LFN
		if(ROOT[i+11] == 0x0F)
			continue;
		// Skip non-root directories
		if(ROOT[i+11] != 0x10)
			continue;

		char dname[12] = {' '};
		memcpy( (uint8_t*)dname, &(ROOT[i]), 11);
		dname[11] = '\0';

		if( !(strcmp(dname, format83dirname)) ){
			Directory* temp = new Directory(dirname, &ROOT[i]);
			*dirdescriptor = temp->dd;
			dirSys.push_back(temp);
			VMMutexRelease(FILE_SYSTEM_MUTEX);
			MachineResumeSignals(&sigstate);
			return VM_STATUS_SUCCESS;
		}
		else
			continue;

	}

	VMMutexRelease(FILE_SYSTEM_MUTEX);
	MachineResumeSignals(&sigstate);
	return VM_STATUS_FAILURE;
}

TVMStatus VMDirectoryRead(int dirdescriptor, SVMDirectoryEntryRef dirent){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	if(!dirent){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	VMMutexAcquire(FILE_SYSTEM_MUTEX, VM_TIMEOUT_INFINITE);

	int index = searchDirectory(dirdescriptor);

	if(index < 0 ){
		VMMutexRelease(FILE_SYSTEM_MUTEX);
		MachineResumeSignals(&sigstate);
		return VM_STATUS_FAILURE;		
	}
	else{
		Directory* temp = dirSys[index];

		if( !temp->isRoot ){
			uint8_t dirInfo[32];

			SVMDirectoryEntry info;

			// Copy the 32 bytes over
			memcpy( dirInfo, temp->clusters[temp->curCluster]->data + temp->offset, 32);
			
			info.DSize = *(uint32_t*)(dirInfo + 28);

			memcpy( (uint8_t*)dirent->DShortFileName, (uint8_t*)dirInfo, 11);

			memcpy( (uint8_t*)&dirent->DAttributes, ((uint8_t*)(dirInfo) + 11), 1);

			string noLFN = "";
			strcpy(info.DLongFileName, noLFN.c_str());
			uint16_t buff = *(uint16_t*)(dirInfo + 14);
			info.DCreate.DHour = buff >> 11;
			info.DCreate.DMinute = (buff >> 5) & 0x3F;
			info.DCreate.DSecond =  (buff & 0x1F) << 1; // Have to shift one over, this is a 2 second count-- we want actual count, so multiply by 2
			info.DCreate.DHundredth = (*(dirInfo + 13)/100)%100;

			buff = *(uint16_t*)(dirInfo + 16);
			info.DCreate.DYear = (buff >> 9) + 1980;
			info.DCreate.DMonth = (buff >> 5) & 0x0F;
			info.DCreate.DDay = buff & 0x1F;

			buff = *(uint16_t*)(dirInfo+18);
			info.DAccess.DYear = (buff >> 9) + 1980;
			info.DAccess.DMonth = (buff >> 5) & 0x0F;
			info.DAccess.DDay = buff & 0x1F;

			buff = *(uint16_t*)(dirInfo+22);
			info.DModify.DHour = buff >> 11;
			info.DModify.DMinute = (buff >> 5) & 0x3F;
			info.DModify.DSecond =  (buff & 0x1F) << 1;
			info.DModify.DHundredth = 0;

			buff = *(uint16_t*)(dirInfo + 24);
			info.DModify.DYear = (buff >> 9) + 1980;
			info.DModify.DMonth = (buff >> 5) & 0x0F;
			info.DModify.DDay = buff & 0x1F;
		}
		else{//In Root...
			uint8_t dirInfo[32];


			while( (ROOT[temp->curCluster*1024 + temp->offset + 11] == 0x0F || (ROOT[temp->curCluster*1024 + temp->offset] == 0xE5))){
				if( (temp->offset + 32) >= 1024 ){
					temp->offset = 0;				
					temp->curCluster++;
				}
				else
					temp->offset += 32;
			}

			if(ROOT[temp->curCluster * 1024 + temp->offset] == 0x00){
				VMMutexRelease(FILE_SYSTEM_MUTEX);
				VMDirectoryRewind(dirdescriptor);
				MachineResumeSignals(&sigstate);
				return VM_STATUS_FAILURE;
			}
			// Copy the 32 bytes over
			memcpy( dirInfo, &ROOT[temp->curCluster * 1024 + temp->offset], 32);
			

			dirent->DSize = *(uint32_t*)(dirInfo + 28);


			memcpy( (uint8_t*)dirent->DShortFileName, (uint8_t*)dirInfo, 11);
			dirent->DShortFileName[11] = '\0';
			memcpy( (uint8_t*)&dirent->DAttributes, ((uint8_t*)(dirInfo) + 11), 1);

			string noLFN = " ";
			strcpy(dirent->DLongFileName, noLFN.c_str());

			uint16_t buff = *(uint16_t*)(dirInfo + 14);
			dirent->DCreate.DHour = buff >> 11;
			dirent->DCreate.DMinute = (buff >> 5) & 0x3F;
			dirent->DCreate.DSecond =  (buff & 0x1F) << 1; // Have to shift one over, this is a 2 second count-- we want actual count, so multiply by 2
			dirent->DCreate.DHundredth = (*(dirInfo + 13)/100)%100;

			buff = *(uint16_t*)(dirInfo + 16);
			dirent->DCreate.DYear = (buff >> 9) + 1980;
			dirent->DCreate.DMonth = (buff >> 5) & 0x0F;
			dirent->DCreate.DDay = buff & 0x1F;

			buff = *(uint16_t*)(dirInfo+18);
			dirent->DAccess.DYear = (buff >> 9) + 1980;
			dirent->DAccess.DMonth = (buff >> 5) & 0x0F;
			dirent->DAccess.DDay = buff & 0x1F;

			buff = *(uint16_t*)(dirInfo+22);
			dirent->DModify.DHour = buff >> 11;
			dirent->DModify.DMinute = (buff >> 5) & 0x3F;
			dirent->DModify.DSecond =  (buff & 0x1F) << 1;
			dirent->DModify.DHundredth = 0;

			buff = *(uint16_t*)(dirInfo + 24);
			dirent->DModify.DYear = (buff >> 9) + 1980;
			dirent->DModify.DMonth = (buff >> 5) & 0x0F;
			dirent->DModify.DDay = buff & 0x1F;

			// dirent = &info;
		}
		

		if( (temp->offset + 32) >= 1024 ){
			temp->offset = 0;
			// Move clusters if possible, otherwise leave it.
			if( (temp->curCluster+1) < temp->clusters.size() )
				temp->curCluster++;
		}
		else
			temp->offset += 32;

		VMMutexRelease(FILE_SYSTEM_MUTEX);
		MachineResumeSignals(&sigstate);
		return VM_STATUS_SUCCESS;		
	}

	VMMutexRelease(FILE_SYSTEM_MUTEX);
	MachineResumeSignals(&sigstate);
	return VM_STATUS_FAILURE;	
}

TVMStatus VMDirectoryRewind(int dirdescriptor){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	VMMutexAcquire(FILE_SYSTEM_MUTEX, VM_TIMEOUT_INFINITE);

	int index = searchDirectory(dirdescriptor);

	if(index < 0 ){
		VMMutexRelease(FILE_SYSTEM_MUTEX);
		MachineResumeSignals(&sigstate);
		return VM_STATUS_FAILURE;		
	}
	else{
		Directory *temp = dirSys[index];
		temp->offset = temp->curCluster = 0;
		VMMutexRelease(FILE_SYSTEM_MUTEX);
		MachineResumeSignals(&sigstate);
		return VM_STATUS_SUCCESS;		
	}
}

TVMStatus VMDirectoryCurrent(char* abspath){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	strcpy( abspath, currentDirectory->pathname.c_str());
	// abspath = (char*)currentDirectory->pathname.c_str();

	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMDirectoryChange(const char* path){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	// uint32_t length = VMStringLength( path );
	char* abspath = new char[256];
	// char* parentPath = new char[256]; //Hard coding length for now

	if( VMFileSystemValidPathName(path) == VM_STATUS_ERROR_INVALID_PARAMETER ){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_FAILURE;
	}

	int index = searchDirectory(ROOT_DIR_DD	);

	// VMFileSystemSimplifyPath(simplePath, dirSys[index]->pathname.c_str(), path );
	// cerr << "Simplepath:"  << simplePath <<  "!" << endl;
	VMFileSystemGetAbsolutePath(abspath, dirSys[index]->pathname.c_str(), path);
	// VMFileSystemDirectoryFromFullPath(parentPath, abspath);


	if( !strcmp(abspath, dirSys[index]->pathname.c_str()) ){
		MachineResumeSignals(&sigstate);
		return VM_STATUS_SUCCESS;			
	}

	MachineResumeSignals(&sigstate);
	return VM_STATUS_FAILURE;	
}

TVMStatus VMDirectoryClose(int dirdescriptor){
	TMachineSignalState sigstate;
	MachineSuspendSignals(&sigstate);

	VMMutexAcquire(FILE_SYSTEM_MUTEX, VM_TIMEOUT_INFINITE);

	int index = searchDirectory(dirdescriptor);

	if(index < 0 ){
		VMMutexRelease(FILE_SYSTEM_MUTEX);
		MachineResumeSignals(&sigstate);
		return VM_STATUS_FAILURE;		
	}
	else{
		Directory *temp = dirSys[index];
		temp->openCount--;
		VMMutexRelease(FILE_SYSTEM_MUTEX);
		MachineResumeSignals(&sigstate);
		return VM_STATUS_SUCCESS;		
	}

	VMMutexRelease(FILE_SYSTEM_MUTEX);
	MachineResumeSignals(&sigstate);
	return VM_STATUS_SUCCESS;	
}

}

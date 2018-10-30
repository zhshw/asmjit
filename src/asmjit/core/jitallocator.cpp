// [AsmJit]
// Complete x86/x64 JIT and Remote Assembler for C++.
//
// [License]
// ZLIB - See LICENSE.md file in the package.

// [Export]
#define ASMJIT_EXPORTS

#include "../core/build.h"
#ifndef ASMJIT_DISABLE_JIT

// [Dependencies]
#include "../core/arch.h"
#include "../core/jitallocator.h"
#include "../core/jitutils.h"
#include "../core/support.h"
#include "../core/zone.h"

ASMJIT_BEGIN_NAMESPACE

// ============================================================================
// [asmjit::BitFlipIterator]
// ============================================================================

//! \internal
//!
//! BitWord[] iterator used by `JitAllocator` that can flip the search pattern during iteration.
template<typename T>
class BitFlipIterator {
public:
  ASMJIT_INLINE BitFlipIterator(const T* data, size_t numBitWords, size_t start = 0, T xorMask = 0) noexcept {
    init(data, numBitWords, start, xorMask);
  }

  ASMJIT_INLINE void init(const T* data, size_t numBitWords, size_t start = 0, T xorMask = 0) noexcept {
    const T* ptr = data + (start / Support::bitSizeOf<T>());
    size_t idx = Support::alignDown(start, Support::bitSizeOf<T>());
    size_t end = numBitWords * Support::bitSizeOf<T>();

    T bitWord = T(0);
    if (idx < end) {
      bitWord = (*ptr++ ^ xorMask) & (Support::allOnes<T>() << (start % Support::bitSizeOf<T>()));
      while (!bitWord && (idx += Support::bitSizeOf<T>()) < end)
        bitWord = *ptr++ ^ xorMask;
    }

    _ptr = ptr;
    _idx = idx;
    _end = end;
    _current = bitWord;
    _xorMask = xorMask;
  }

  ASMJIT_INLINE bool hasNext() const noexcept {
    return _current != T(0);
  }

  ASMJIT_INLINE size_t next() noexcept {
    T bitWord = _current;
    ASMJIT_ASSERT(bitWord != T(0));

    uint32_t bit = ctz(bitWord);
    bitWord ^= T(1u) << bit;

    size_t n = _idx + bit;
    while (!bitWord && (_idx += Support::bitSizeOf<T>()) < _end)
      bitWord = *_ptr++ ^ _xorMask;

    _current = bitWord;
    return n;
  }

  ASMJIT_INLINE size_t nextAndFlip() noexcept {
    T bitWord = _current;
    ASMJIT_ASSERT(bitWord != T(0));

    uint32_t bit = Support::ctz(bitWord);
    bitWord ^= Support::allOnes<T>() << bit;
    _xorMask ^= Support::allOnes<T>();

    size_t n = _idx + bit;
    while (!bitWord && (_idx += Support::bitSizeOf<T>()) < _end)
      bitWord = *_ptr++ ^ _xorMask;

    _current = bitWord;
    return n;
  }

  ASMJIT_INLINE size_t peekNext() const noexcept {
    ASMJIT_ASSERT(_current != T(0));
    return _idx + Support::ctz(_current);
  }

  const T* _ptr;
  size_t _idx;
  size_t _end;
  T _current;
  T _xorMask;
};

// ============================================================================
// [asmjit::JitAllocator::TypeDefs]
// ============================================================================

typedef JitAllocator::Block Block;

// ============================================================================
// [asmjit::JitAllocator - Helpers]
// ============================================================================

static inline uint32_t JitAllocator_defaultFillPattern() noexcept {
  // X86 and X86_64 - 4x 'int3' instruction.
  if (ASMJIT_ARCH_X86)
    return 0xCCCCCCCCu;

  // Unknown...
  return 0u;
}

static inline size_t JitAllocator_sizeToPoolId(const JitAllocator* self, size_t size) noexcept {
  ASMJIT_UNUSED(self);

  size_t poolId = size_t(JitAllocator::kPoolCount - 1);
  size_t granularity = size_t(JitAllocator::kMinGranularity) << poolId;

  while (poolId) {
    if (Support::alignUp(size, granularity) == size)
      break;
    poolId--;
    granularity >>= 1;
  }
  return poolId;
}

static inline size_t JitAllocator_bitVectorSizeToByteSize(uint32_t areaSize) noexcept {
  using Support::kBitWordSizeInBits;
  return ((areaSize + kBitWordSizeInBits - 1u) / kBitWordSizeInBits) * sizeof(Support::BitWord);
}

static inline size_t JitAllocator_calculateIdealBlockSize(JitAllocator::Pool* pool, size_t allocationSize) noexcept {
  uint32_t kMaxSizeShift = Support::constCtz(JitAllocator::kMaxBlockSize) -
                           Support::constCtz(JitAllocator::kMinBlockSize) ;

  size_t blockSize = size_t(JitAllocator::kMinBlockSize) << Support::min<uint32_t>(kMaxSizeShift, pool->_blockCount);
  if (blockSize < allocationSize)
    blockSize = Support::alignUp(allocationSize, blockSize);
  return blockSize;
}

ASMJIT_FAVOR_SPEED static void JitAllocator_fillPattern(void* mem, uint32_t pattern, size_t sizeInBytes) noexcept {
  size_t n = sizeInBytes / 4u;
  uint32_t* p = static_cast<uint32_t*>(mem);

  for (size_t i = 0; i < n; i++)
    p[i] = pattern;
}

// Allocate a new `JitAllocator::Block` for the given `blockSize`.
static Block* JitAllocator_newBlock(JitAllocator* self, JitAllocator::Pool* pool, size_t blockSize) noexcept {
  using Support::BitWord;
  using Support::kBitWordSizeInBits;

  uint32_t areaSize = uint32_t((blockSize + pool->granularity() - 1) >> pool->_granularityLog2);
  uint32_t numBitWords = (areaSize + kBitWordSizeInBits - 1u) / kBitWordSizeInBits;

  Block* block = static_cast<Block*>(::malloc(sizeof(Block)));
  BitWord* bitWords = static_cast<BitWord*>(::malloc(size_t(numBitWords) * 2 * sizeof(BitWord)));
  uint8_t* virtMem = static_cast<uint8_t*>(JitUtils::virtualAlloc(blockSize, JitUtils::kVirtMemWriteExecute));

  // Out of memory.
  if (ASMJIT_UNLIKELY(!block || !bitWords || !virtMem)) {
    if (virtMem) JitUtils::virtualRelease(virtMem, blockSize);
    if (bitWords) ::free(bitWords);
    if (block) ::free(block);
    return nullptr;
  }

  // Fill memory if secure mode is enabled.
  if (self->hasFlag(JitAllocator::kFlagSecureMode))
    JitAllocator_fillPattern(virtMem, self->fillPattern(), blockSize);

  ::memset(bitWords, 0, size_t(numBitWords) * 2 * sizeof(BitWord));
  return new(block) Block(pool, virtMem, blockSize, bitWords, bitWords + numBitWords, areaSize);
}

static void JitAllocator_deleteBlock(JitAllocator* self, JitAllocator::Block* block) noexcept {
  ASMJIT_UNUSED(self);

  JitUtils::virtualRelease(block->virtMem(), block->blockSize());
  ::free(block->_usedBitVector);
  ::free(block);
}

static void JitAllocator_insertBlock(JitAllocator* self, JitAllocator::Block* block) noexcept {
  JitAllocator::Pool* pool = block->pool();

  if (!pool->_cursor)
    pool->_cursor = block;

  // Add to RBTree and List.
  self->_tree.insert(block);
  pool->_blocks.append(block);

  // Update statistics.
  pool->_blockCount++;
  pool->_totalAreaSize += block->areaSize();
  pool->_totalOverheadBytes += sizeof(Block) + JitAllocator_bitVectorSizeToByteSize(block->areaSize()) * 2u;
}

static void JitAllocator_removeBlock(JitAllocator* self, JitAllocator::Block* block) noexcept {
  JitAllocator::Pool* pool = block->pool();

  // Remove from RBTree and List.
  if (pool->_cursor == block)
    pool->_cursor = block->hasPrev() ? block->prev() : block->next();

  self->_tree.remove(block);
  pool->_blocks.unlink(block);

  // Update statistics.
  pool->_blockCount--;
  pool->_totalAreaSize -= block->areaSize();
  pool->_totalOverheadBytes -= sizeof(Block) + JitAllocator_bitVectorSizeToByteSize(block->areaSize()) * 2u;
}

// ============================================================================
// [asmjit::JitAllocator - Construction / Destruction]
// ============================================================================

JitAllocator::JitAllocator() noexcept {
  JitUtils::MemInfo memInfo = JitUtils::memInfo();

  _flags = 0;
  _pageSize = memInfo.pageSize;
  _blockSize = memInfo.pageGranularity;
  _fillPattern = JitAllocator_defaultFillPattern();

  for (size_t poolId = 0; poolId < kPoolCount; poolId++)
    _pools[poolId].setGranularity(uint32_t(kMinGranularity) << poolId);
}

JitAllocator::~JitAllocator() noexcept {
  reset();
}

// ============================================================================
// [asmjit::JitAllocator - Reset]
// ============================================================================

void JitAllocator::reset() noexcept {
  for (size_t poolId = 0; poolId < kPoolCount; poolId++) {
    Pool& pool = _pools[poolId];
    Block* block = pool._blocks.first();

    while (block) {
      Block* next = block->next();
      JitAllocator_deleteBlock(this, block);
      block = next;
    }

    pool.reset();
  }
  _tree.reset();
}

// ============================================================================
// [asmjit::JitAllocator - Statistics]
// ============================================================================

JitAllocator::Statistics JitAllocator::statistics() const noexcept {
  Statistics statistics;
  statistics.reset();

  ScopedLock locked(_lock);
  for (size_t poolId = 0; poolId < kPoolCount; poolId++) {
    const Pool& pool = _pools[poolId];
    statistics._blockCount   += size_t(pool._blockCount);
    statistics._reservedSize += size_t(pool._totalAreaSize) * pool.granularity();
    statistics._usedSize     += size_t(pool._totalAreaUsed) * pool.granularity();
    statistics._overheadSize += size_t(pool._totalOverheadBytes);
  }
  return statistics;
}

// ============================================================================
// [asmjit::JitAllocator - Alloc / Release]
// ============================================================================

void* JitAllocator::alloc(size_t size) noexcept {
  constexpr uint32_t kNoIndex = std::numeric_limits<uint32_t>::max();

  // Align to minimum granularity by default.
  size = Support::alignUp<size_t>(size, kMinGranularity);
  if (ASMJIT_UNLIKELY(size == 0 || size > std::numeric_limits<uint32_t>::max() / 2))
    return nullptr;

  ScopedLock locked(_lock);
  Pool* pool = &_pools[JitAllocator_sizeToPoolId(this, size)];

  uint32_t areaIndex = kNoIndex;
  uint32_t areaSize = uint32_t(pool->areaSizeFromByteSize(size));

  // Try to find the requested memory area in existing blocks.
  Block* block = pool->_blocks.first();
  if (block) {
    Block* initial = block;
    do {
      Block* next = block->hasNext() ? block->next() : pool->_blocks.first();
      if (block->areaAvailable() >= areaSize) {
        if (block->hasFlag(Block::kFlagDirty) || block->largestUnusedArea() >= areaSize) {
          uint32_t blockAreaSize = block->areaSize();
          uint32_t searchStart = block->_searchStart;
          uint32_t searchEnd = block->_searchEnd;

          BitFlipIterator<Support::BitWord> it(
            block->_usedBitVector,
            pool->bitWordCountFromAreaSize(searchEnd),
            searchStart,
            Support::allOnes<Support::BitWord>());

          // If there is unused area available then there has to be at least one match.
          ASMJIT_ASSERT(it.hasNext());

          uint32_t bestArea = blockAreaSize;
          uint32_t largestArea = 0;

          uint32_t holeIndex = uint32_t(it.peekNext());
          uint32_t holeEnd = holeIndex;

          searchStart = holeIndex;
          do {
            holeIndex = uint32_t(it.nextAndFlip());
            if (holeIndex >= searchEnd) break;

            holeEnd = it.hasNext() ? Support::min(searchEnd, uint32_t(it.nextAndFlip())) : searchEnd;
            uint32_t holeSize = holeEnd - holeIndex;

            if (holeSize >= areaSize && bestArea >= holeSize) {
              largestArea = Support::max(largestArea, bestArea);
              bestArea = holeSize;
              areaIndex = holeIndex;
            }
            else {
              largestArea = Support::max(largestArea, holeSize);
            }
          } while (it.hasNext());
          searchEnd = holeEnd;

          // Because we have traversed the entire block, we can now mark the
          // largest unused area that can be used to cache the next traversal.
          block->_searchStart = searchStart;
          block->_searchEnd = searchEnd;
          block->_largestUnusedArea = largestArea;
          block->clearFlags(Block::kFlagDirty);

          if (areaIndex != kNoIndex) {
            if (searchStart == areaIndex)
              block->_searchStart += areaSize;
            break;
          }
        }
      }

      block = next;
    } while (block != initial);
  }

  // Allocate a new block if there is no region of a required width.
  if (areaIndex == kNoIndex) {
    size_t blockSize = JitAllocator_calculateIdealBlockSize(pool, size);
    block = JitAllocator_newBlock(this, pool, blockSize);

    if (ASMJIT_UNLIKELY(!block))
      return nullptr;

    JitAllocator_insertBlock(this, block);
    areaIndex = 0;
    block->_searchStart = areaSize;
    block->_largestUnusedArea = block->areaSize() - areaSize;
  }

  // Update statistics.
  block->increaseUsedArea(areaSize);

  // Handle special cases.
  if (block->areaAvailable() == 0) {
    // The whole block is filled.
    block->_searchStart = block->areaSize();
    block->_searchEnd = 0;
    block->_largestUnusedArea = 0;
    block->clearFlags(Block::kFlagDirty);
  }

  // Mark the newly allocated space as occupied and also the sentinel.
  Support::bitVectorFill(block->_usedBitVector, areaIndex, areaSize);
  Support::bitVectorSetBit(block->_stopBitVector, areaIndex + areaSize - 1, true);

  // Return a pointer to allocated memory.
  uint8_t* result = block->virtMem() + pool->byteSizeFromAreaSize(areaIndex);
  ASMJIT_ASSERT(result >= block->virtMem());
  ASMJIT_ASSERT(result <= block->virtMem() + block->blockSize() - size);
  return result;
}

Error JitAllocator::release(void* p) noexcept {
  if (ASMJIT_UNLIKELY(!p))
    return DebugUtils::errored(kErrorInvalidArgument);

  ScopedLock locked(_lock);
  Block* block = _tree.get(static_cast<uint8_t*>(p));

  if (ASMJIT_UNLIKELY(!block))
    return DebugUtils::errored(kErrorInvalidState);

  // Offset relative to the start of the block.
  Pool* pool = block->pool();
  size_t offset = (size_t)((uint8_t*)p - block->virtMem());

  // The first bit representing the allocated area and its size.
  uint32_t areaIndex = uint32_t(offset >> pool->_granularityLog2);
  uint32_t areaLast = uint32_t(Support::bitVectorIndexOf(block->_stopBitVector, areaIndex, true));
  uint32_t areaSize = areaLast - areaIndex + 1;

  // Update the search region and statistics.
  block->_searchStart = Support::min(block->_searchStart, areaIndex);
  block->_searchEnd = Support::max(block->_searchEnd, areaLast + 1);
  block->addFlags(Block::kFlagDirty);
  block->decreaseUsedArea(areaSize);

  // Clear all occupied bits and also the sentinel.
  Support::bitVectorClear(block->_usedBitVector, areaIndex, areaSize);
  Support::bitVectorSetBit(block->_stopBitVector, areaLast, false);

  // Fill the released memory if the secure mode is enabled.
  if (hasFlag(kFlagSecureMode))
    JitAllocator_fillPattern(block->virtMem() + areaIndex * pool->granularity(), fillPattern(), areaSize * pool->granularity());

  // Release the whole block if it became empty.
  if (block->areaUsed() == 0) {
    JitAllocator_removeBlock(this, block);
    JitAllocator_deleteBlock(this, block);
  }

  return kErrorOk;
}

Error JitAllocator::shrink(void* p, size_t newSize) noexcept {
  if (ASMJIT_UNLIKELY(!p))
    return DebugUtils::errored(kErrorInvalidArgument);

  if (ASMJIT_UNLIKELY(newSize == 0))
    return release(p);

  ScopedLock locked(_lock);
  Block* block = _tree.get(static_cast<uint8_t*>(p));

  if (ASMJIT_UNLIKELY(!block))
    return DebugUtils::errored(kErrorInvalidState);

  // Offset relative to the start of the block.
  Pool* pool = block->pool();
  size_t offset = (size_t)((uint8_t*)p - block->virtMem());

  // The first bit representing the allocated area and its size.
  uint32_t areaIndex = uint32_t(offset >> pool->_granularityLog2);
  uint32_t areaOldSize = uint32_t(Support::bitVectorIndexOf(block->_stopBitVector, areaIndex, true)) + 1 - areaIndex;
  uint32_t areaNewSize = pool->areaSizeFromByteSize(newSize);

  if (ASMJIT_UNLIKELY(areaNewSize > areaOldSize))
    return DebugUtils::errored(kErrorInvalidState);

  uint32_t areaDiff = areaOldSize - areaNewSize;
  if (!areaDiff)
    return kErrorOk;

  // Update the search region and statistics.
  block->_searchStart = Support::min(block->_searchStart, areaIndex + areaNewSize);
  block->_searchEnd = Support::max(block->_searchEnd, areaIndex + areaOldSize);
  block->addFlags(Block::kFlagDirty);
  block->decreaseUsedArea(areaDiff);

  // Unmark the released space and move the sentinel.
  Support::bitVectorClear(block->_usedBitVector, areaIndex + areaNewSize, areaDiff);
  Support::bitVectorSetBit(block->_stopBitVector, areaIndex + areaOldSize - 1, false);
  Support::bitVectorSetBit(block->_stopBitVector, areaIndex + areaNewSize - 1, true);

  // Fill released memory if the secure mode is enabled.
  if (hasFlag(kFlagSecureMode))
    JitAllocator_fillPattern(
      block->virtMem() + (areaIndex + areaOldSize) * pool->granularity(),
      fillPattern(),
      areaDiff * pool->granularity());

  return kErrorOk;
}

// ============================================================================
// [asmjit::JitAllocator - Unit]
// ============================================================================

#if defined(ASMJIT_BUILD_TEST)
// A pseudo random number generator based on a paper by Sebastiano Vigna:
//   http://vigna.di.unimi.it/ftp/papers/xorshiftplus.pdf
class Random {
public:
  // Constants suggested as `23/18/5`.
  enum Steps : uint32_t {
    kStep1_SHL = 23,
    kStep2_SHR = 18,
    kStep3_SHR = 5
  };

  inline explicit Random(uint64_t seed = 0) noexcept { reset(seed); }
  inline Random(const Random& other) noexcept = default;

  inline void reset(uint64_t seed = 0) noexcept {
    // The number is arbitrary, it means nothing.
    constexpr uint64_t kZeroSeed = 0x1F0A2BE71D163FA0u;

    // Generate the state data by using splitmix64.
    for (uint32_t i = 0; i < 2; i++) {
      seed += 0x9E3779B97F4A7C15u;
      uint64_t x = seed;
      x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9u;
      x = (x ^ (x >> 27)) * 0x94D049BB133111EBu;
      x = (x ^ (x >> 31));
      _state[i] = x != 0 ? x : kZeroSeed;
    }
  }

  inline uint32_t nextUInt32() noexcept {
    return uint32_t(nextUInt64() >> 32);
  }

  inline uint64_t nextUInt64() noexcept {
    uint64_t x = _state[0];
    uint64_t y = _state[1];

    x ^= x << kStep1_SHL;
    y ^= y >> kStep3_SHR;
    x ^= x >> kStep2_SHR;
    x ^= y;

    _state[0] = y;
    _state[1] = x;
    return x + y;
  }

  uint64_t _state[2];
};

// Helper class to verify that JitAllocator doesn't return addresses that overlap.
class JitAllocatorWrapper {
public:
  inline JitAllocatorWrapper() noexcept
    : _zone(1024 * 1024),
      _heap(&_zone) {}

  // Address to a memory region of a given size.
  class Range {
  public:
    inline Range(uint8_t* addr, size_t size) noexcept
      : addr(addr),
        size(size) {}
    uint8_t* addr;
    size_t size;
  };

  // Based on JitAllocator::Block, serves our purpose well...
  class Record : public ZoneTreeNodeT<Record>,
                 public Range {
  public:
    inline Record(uint8_t* addr, size_t size)
      : ZoneTreeNodeT<Record>(),
        Range(addr, size) {}

    inline bool operator<(const Record& other) const noexcept { return addr < other.addr; }
    inline bool operator>(const Record& other) const noexcept { return addr > other.addr; }

    inline bool operator<(const uint8_t* key) const noexcept { return addr + size <= key; }
    inline bool operator>(const uint8_t* key) const noexcept { return addr > key; }
  };

  void _insert(void* p_, size_t size) noexcept {
    uint8_t* p = static_cast<uint8_t*>(p_);
    uint8_t* pEnd = p + size - 1;

    Record* record;

    record = _records.get(p);
    if (record)
      EXPECT(record == nullptr,
             "Address [%p:%p] collides with a newly allocated [%p:%p]\n", record->addr, record->addr + record->size, p, p + size);

    record = _records.get(pEnd);
    if (record)
      EXPECT(record == nullptr,
             "Address [%p:%p] collides with a newly allocated [%p:%p]\n", record->addr, record->addr + record->size, p, p + size);

    record = _heap.newT<Record>(p, size);
    EXPECT(record != nullptr,
           "Out of memory, cannot allocate 'Record'");

    _records.insert(record);
  }

  void _remove(void* p) noexcept {
    Record* record = _records.get(static_cast<uint8_t*>(p));
    EXPECT(record != nullptr,
           "Address [%p] doesn't exist\n", p);

    _records.remove(record);
    _heap.release(record, sizeof(Record));
  }

  void* alloc(size_t size) noexcept {
    void* p = _allocator.alloc(size);
    EXPECT(p != nullptr,
           "JitAllocator failed to allocate '%u' bytes\n", unsigned(size));

    _insert(p, size);
    return p;
  }

  void release(void* p) noexcept {
    _remove(p);
    EXPECT(_allocator.release(p) == kErrorOk,
           "JitAllocator failed to release '%p'\n", p);
  }

  Zone _zone;
  ZoneAllocator _heap;
  ZoneTree<Record> _records;
  JitAllocator _allocator;
};

static void JitAllocatorTest_shuffle(void** ptrArray, size_t count, Random& prng) noexcept {
  for (size_t i = 0; i < count; ++i)
    std::swap(ptrArray[i], ptrArray[size_t(prng.nextUInt32() % count)]);
}

static void JitAllocatorTest_usage(JitAllocator& allocator) noexcept {
  JitAllocator::Statistics stats = allocator.statistics();
  INFO("Block Count       : %9llu [Blocks]"        , (unsigned long long)(stats.blockCount()));
  INFO("Reserved (VirtMem): %9llu [Bytes]"         , (unsigned long long)(stats.reservedSize()));
  INFO("Used     (VirtMem): %9llu [Bytes] (%.1f%%)", (unsigned long long)(stats.usedSize()), stats.usedSizeAsPercent());
  INFO("Overhead (HeapMem): %9llu [Bytes] (%.1f%%)", (unsigned long long)(stats.overheadSize()), stats.overheadSizeAsPercent());
}

UNIT(asmjit_core_jit_allocator) {
  JitAllocatorWrapper wrapper;
  Random prng(100);

  size_t i;
  size_t kCount = 200000;

  INFO("BitFlipIterator<uint32_t>");
  {
    static const uint32_t bits[] = { 0x80000000u, 0x80000000u, 0x00000000u, 0x80000000u };
    BitFlipIterator<uint32_t> it(bits, ASMJIT_ARRAY_SIZE(bits));

    EXPECT(it.hasNext());
    EXPECT(it.nextAndFlip() == 31);
    EXPECT(it.hasNext());
    EXPECT(it.nextAndFlip() == 32);
    EXPECT(it.hasNext());
    EXPECT(it.nextAndFlip() == 63);
    EXPECT(it.hasNext());
    EXPECT(it.nextAndFlip() == 64);
    EXPECT(it.hasNext());
    EXPECT(it.nextAndFlip() == 127);
    EXPECT(!it.hasNext());
  }

  INFO("BitFlipIterator<uint64_t>");
  {
    static const uint64_t bits[] = { 0xFFFFFFFFFFFFFFFFu, 0xFFFFFFFFFFFFFFFF, 0, 0 };
    BitFlipIterator<uint64_t> it(bits, ASMJIT_ARRAY_SIZE(bits));

    EXPECT(it.hasNext());
    EXPECT(it.nextAndFlip() == 0);
    EXPECT(it.hasNext());
    EXPECT(it.nextAndFlip() == 128);
    EXPECT(!it.hasNext());
  }

  INFO("Memory alloc/release test - %d allocations", kCount);

  void** ptrArray = (void**)::malloc(sizeof(void*) * size_t(kCount));
  EXPECT(ptrArray != nullptr,
        "Couldn't allocate '%u' bytes for pointer-array", unsigned(sizeof(void*) * size_t(kCount)));

  INFO("Allocating virtual memory...");
  for (i = 0; i < kCount; i++)
    ptrArray[i] = wrapper.alloc((prng.nextUInt32() % 1000) + 8);
  JitAllocatorTest_usage(wrapper._allocator);

  INFO("Releasing virtual memory...");
  for (i = 0; i < kCount; i++)
    wrapper.release(ptrArray[i]);
  JitAllocatorTest_usage(wrapper._allocator);

  INFO("Allocating virtual memory...", kCount);
  for (i = 0; i < kCount; i++)
    ptrArray[i] = wrapper.alloc((prng.nextUInt32() % 1000) + 8);
  JitAllocatorTest_usage(wrapper._allocator);

  INFO("Shuffling...");
  JitAllocatorTest_shuffle(ptrArray, unsigned(kCount), prng);

  INFO("Releasing 50% blocks...");
  for (i = 0; i < kCount / 2; i++)
    wrapper.release(ptrArray[i]);
  JitAllocatorTest_usage(wrapper._allocator);

  INFO("Allocating 50% blocks again...");
  for (i = 0; i < kCount / 2; i++)
    ptrArray[i] = wrapper.alloc((prng.nextUInt32() % 1000) + 8);
  JitAllocatorTest_usage(wrapper._allocator);

  INFO("Releasing virtual memory...");
  for (i = 0; i < kCount; i++)
    wrapper.release(ptrArray[i]);
  JitAllocatorTest_usage(wrapper._allocator);

  ::free(ptrArray);
}
#endif

ASMJIT_END_NAMESPACE

#endif

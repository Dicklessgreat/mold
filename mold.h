#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Timer.h"
#include "tbb/concurrent_hash_map.h"
#include "tbb/global_control.h"
#include "tbb/parallel_for_each.h"
#include "tbb/parallel_invoke.h"
#include "tbb/parallel_reduce.h"
#include "tbb/spin_mutex.h"
#include "tbb/task_group.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#define SECTOR_SIZE 512
#define PAGE_SIZE 4096

#define LIKELY(x)   __builtin_expect((x), 1)
#define UNLIKELY(x) __builtin_expect((x), 0)

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

using llvm::ArrayRef;
using llvm::ErrorOr;
using llvm::Error;
using llvm::Expected;
using llvm::MemoryBufferRef;
using llvm::SmallVector;
using llvm::StringRef;
using llvm::Twine;
using llvm::object::ELF64LE;
using llvm::object::ELFFile;

class Symbol;
class InputSection;
class ObjectFile;
class OutputChunk;
class OutputSection;
class MergedSection;

struct Config {
  StringRef output;
  bool print_map = false;
  bool is_static = false;
};

inline Config config;

[[noreturn]] inline void error(const Twine &msg) {
  static std::mutex mu;
  std::lock_guard lock(mu);

  llvm::errs() << msg << "\n";
  exit(1);
}

template <class T> T check(ErrorOr<T> e) {
  if (auto ec = e.getError())
    error(ec.message());
  return std::move(*e);
}

template <class T> T check(Expected<T> e) {
  if (!e)
    error(llvm::toString(e.takeError()));
  return std::move(*e);
}

template <class T>
T check2(ErrorOr<T> e, llvm::function_ref<std::string()> prefix) {
  if (auto ec = e.getError())
    error(prefix() + ": " + ec.message());
  return std::move(*e);
}

template <class T>
T check2(Expected<T> e, llvm::function_ref<std::string()> prefix) {
  if (!e)
    error(prefix() + ": " + toString(e.takeError()));
  return std::move(*e);
}

inline std::string toString(const Twine &s) { return s.str(); }

#define CHECK(E, S) check2((E), [&] { return toString(S); })

std::string toString(ObjectFile *);

template<typename T, typename Callable>
static void for_each(T &arr, Callable callback) {
#if 1
  tbb::parallel_for_each(arr, callback);
#else
  std::for_each(arr.begin(), arr.end(), callback);
#endif
}

//
// Interned string
//

namespace tbb {
template<>
struct tbb_hash_compare<StringRef> {
  static size_t hash(const StringRef& k) {
    return llvm::hash_value(k);
  }

  static bool equal(const StringRef& k1, const StringRef& k2) {
    return k1 == k2;
  }
};
}

template<typename ValueT>
class ConcurrentMap {
public:
  typedef tbb::concurrent_hash_map<StringRef, ValueT> MapT;

  ValueT *insert(StringRef key, const ValueT &val) {
    typename MapT::const_accessor acc;
    map.insert(acc, std::make_pair(key, val));
    return const_cast<ValueT *>(&acc->second);
  }

  size_t size() const { return map.size(); }

private:
  MapT map;
};

//
// Symbol
//

struct StringPiece {
  StringPiece(StringRef data, InputSection *isec)
    : data(data), isec(isec) {}

  StringPiece(const StringPiece &other)
    : data(other.data), isec(other.isec.load()),
      output_offset(other.output_offset) {}

  inline u64 get_addr() const;

  StringRef data;
  std::atomic<InputSection *> isec;
  u32 output_offset = 0;
};

struct StringPieceRef {
  StringPiece *piece = nullptr;
  u32 input_offset = 0;
  u32 addend = 0;
};

class Symbol {
public:
  Symbol(StringRef name, ObjectFile *file = nullptr)
    : name(name), file(file), is_placeholder(false), is_dso(false),
      is_weak(false), is_undef_weak(false), traced(false) {}

  Symbol(const Symbol &other) : Symbol(other.name, other.file) {}

  static Symbol *intern(StringRef name) {
    static ConcurrentMap<Symbol> map;
    return map.insert(name, Symbol(name));    
  }

  inline u64 get_addr() const;

  StringRef name;
  ObjectFile *file = nullptr;
  InputSection *input_section = nullptr;
  StringPieceRef piece_ref;

  u64 value = 0;
  u32 got_offset = 0;
  u32 gotplt_offset = 0;
  u32 gottp_offset = 0;
  u32 plt_offset = 0;
  u32 relplt_offset = 0;

  u32 shndx = 0;

  tbb::spin_mutex mu;

  u8 is_placeholder : 1;
  u8 is_dso : 1;
  u8 is_weak : 1;
  u8 is_undef_weak : 1;
  u8 traced : 1;

  enum { NEEDS_GOT = 1, NEEDS_GOTTP = 2, NEEDS_PLT = 4 };
  std::atomic_uint8_t flags = ATOMIC_VAR_INIT(0);

  u8 visibility = 0;
  u8 type = llvm::ELF::STT_NOTYPE;
};

inline std::string toString(Symbol sym) {
  return (StringRef(sym.name) + "(" + toString(sym.file) + ")").str();
}

//
// input_sections.cc
//

class InputSection {
public:
  InputSection(ObjectFile *file, const ELF64LE::Shdr &shdr, StringRef name);

  void copy_to(u8 *buf);
  void scan_relocations();

  ObjectFile *file;
  OutputSection *output_section;
  ArrayRef<ELF64LE::Rela> rels;
  const ELF64LE::Shdr &shdr;

  StringRef name;
  u32 offset;

  MergedSection *merged_section = nullptr;
  std::vector<StringPieceRef> pieces;
  std::vector<StringPieceRef> rel_pieces;
  u32 merged_offset = 0;
  u32 merged_size = 0;
};

std::string toString(InputSection *isec);

inline u64 align_to(u64 val, u64 align) {
  assert(__builtin_popcount(align) == 1);
  return (val + align - 1) & ~(align - 1);
}

//
// output_chunks.cc
//

class OutputChunk {
public:
  OutputChunk() { shdr.sh_addralign = 1; }

  virtual void copy_to(u8 *buf) {}

  StringRef name;
  int shndx = 0;
  bool starts_new_ptload = false;
  ELF64LE::Shdr shdr = {};
  std::vector<InputSection *> sections;
};

// ELF header
class OutputEhdr : public OutputChunk {
public:
  OutputEhdr() {
    shdr.sh_flags = llvm::ELF::SHF_ALLOC;
    shdr.sh_size = sizeof(ELF64LE::Ehdr);
  }

  void copy_to(u8 *buf) override;
};

// Section header
class OutputShdr : public OutputChunk {
public:
  OutputShdr() { shdr.sh_flags = llvm::ELF::SHF_ALLOC; }

  void copy_to(u8 *buf) override {
    auto *p = (ELF64LE::Shdr *)(buf + shdr.sh_offset);
    for (ELF64LE::Shdr *ent : entries)
      *p++ = *ent;
  }

  void set_entries(std::vector<ELF64LE::Shdr *> vec) {
    shdr.sh_size = vec.size() * sizeof(ELF64LE::Shdr);
    entries = std::move(vec);
  }

  std::vector<ELF64LE::Shdr *> entries;
};

// Program header
class OutputPhdr : public OutputChunk {
public:
  struct Entry {
    ELF64LE::Phdr phdr;
    std::vector<OutputChunk *> members;
  };

  OutputPhdr() { shdr.sh_flags = llvm::ELF::SHF_ALLOC; }
  void copy_to(u8 *buf) override;

  void set_entries(std::vector<Entry> vec) {
    shdr.sh_size = vec.size() * sizeof(ELF64LE::Phdr);
    entries = std::move(vec);
  }

  std::vector<Entry> entries;
};

// Sections
class OutputSection : public OutputChunk {
public:
  static OutputSection *get_instance(StringRef name, u64 flags, u32 type);

  OutputSection(StringRef name, u64 flags, u32 type) {
    this->name = name;
    shdr.sh_flags = flags;
    shdr.sh_type = type;
    idx = instances.size();
    instances.push_back(this);
  }

  void copy_to(u8 *buf) override {
    if (shdr.sh_type != llvm::ELF::SHT_NOBITS)
      for_each(sections, [&](InputSection *isec) { isec->copy_to(buf); });
  }

  bool empty() const {
    if (!sections.empty())
      for (InputSection *isec : sections)
        if (isec->shdr.sh_size)
          return false;
    return true;
  }

  static inline std::vector<OutputSection *> instances;

  u32 idx;
};

class InterpSection : public OutputChunk {
public:
  InterpSection() {
    name = ".interp";
    shdr.sh_flags = llvm::ELF::SHF_ALLOC;
    shdr.sh_type = llvm::ELF::SHT_PROGBITS;
    shdr.sh_size = sizeof(path);
  }

  void copy_to(u8 *buf) override {
    memcpy(buf + shdr.sh_offset, path, sizeof(path));
  }

private:
  static constexpr char path[] = "/lib64/ld-linux-x86-64.so.2";
};


class GotSection : public OutputChunk {
public:
  GotSection(StringRef name) {
    this->name = name;
    shdr.sh_flags = llvm::ELF::SHF_ALLOC | llvm::ELF::SHF_WRITE;
    shdr.sh_type = llvm::ELF::SHT_PROGBITS;
    shdr.sh_addralign = 8;
  }
};

class PltSection : public OutputChunk {
public:
  PltSection() {
    this->name = ".plt";
    shdr.sh_flags = llvm::ELF::SHF_ALLOC | llvm::ELF::SHF_EXECINSTR;
    shdr.sh_type = llvm::ELF::SHT_PROGBITS;
    shdr.sh_addralign = 8;
  }

  void write_entry(u8 *buf, u32 value) {
    buf[0] = 0xff;
    buf[1] = 0x25;
    *(u32 *)(buf + 2) = value;
  }
};

class RelPltSection : public OutputChunk {
public:
  RelPltSection() {
    this->name = ".rela.plt";
    shdr.sh_flags = llvm::ELF::SHF_ALLOC;
    shdr.sh_type = llvm::ELF::SHT_RELA;
    shdr.sh_entsize = sizeof(ELF64LE::Rela);
    shdr.sh_addralign = 8;
  }
};

class ShstrtabSection : public OutputChunk {
public:
  ShstrtabSection() {
    this->name = ".shstrtab";
    contents = '\0';
    shdr.sh_flags = 0;
    shdr.sh_type = llvm::ELF::SHT_STRTAB;
    shdr.sh_size = 1;
  }

  u64 add_string(StringRef s) {
    u64 ret = contents.size();
    contents += s.str();
    contents += '\0';
    shdr.sh_size = contents.size();
    return ret;
  }

  void copy_to(u8 *buf) override {
    memcpy(buf + shdr.sh_offset, &contents[0], contents.size());
  }

private:
  std::string contents;
};

class SymtabSection : public OutputChunk {
public:
  SymtabSection() {
    this->name = ".symtab";
    shdr.sh_flags = 0;
    shdr.sh_type = llvm::ELF::SHT_SYMTAB;
    shdr.sh_entsize = sizeof(ELF64LE::Sym);
    shdr.sh_addralign = 8;
    shdr.sh_size = sizeof(ELF64LE::Sym);
    contents.push_back({});
  }

private:
  std::vector<ELF64LE::Sym> contents;
};

class StrtabSection : public OutputChunk {
public:
  StrtabSection() {
    this->name = ".strtab";
    shdr.sh_flags = 0;
    shdr.sh_type = llvm::ELF::SHT_STRTAB;
    shdr.sh_size = 1;
  }
};

class MergedSection : public OutputChunk {
public:
  static MergedSection *get_instance(StringRef name, u64 flags, u32 type);

  static inline std::vector<MergedSection *> instances;

  ConcurrentMap<StringPiece> map;

private:
  MergedSection(StringRef name, u64 flags, u32 type);
};

bool is_c_identifier(StringRef name);

namespace out {
inline OutputEhdr *ehdr;
inline OutputShdr *shdr;
inline OutputPhdr *phdr;
inline InterpSection *interp;
inline GotSection *got;
inline GotSection *gotplt;
inline PltSection *plt;
inline RelPltSection *relplt;
inline ShstrtabSection *shstrtab;
inline SymtabSection *symtab;
inline StrtabSection *strtab;

inline u64 tls_end;

inline Symbol *__bss_start;
inline Symbol *__ehdr_start;
inline Symbol *__rela_iplt_start;
inline Symbol *__rela_iplt_end;
inline Symbol *__init_array_start;
inline Symbol *__init_array_end;
inline Symbol *__fini_array_start;
inline Symbol *__fini_array_end;
inline Symbol *__preinit_array_start;
inline Symbol *__preinit_array_end;
inline Symbol *end;
inline Symbol *_end;
inline Symbol *etext;
inline Symbol *_etext;
inline Symbol *edata;
inline Symbol *_edata;
}

inline u64 Symbol::get_addr() const {
  if (piece_ref.piece)
    return piece_ref.piece->get_addr() + piece_ref.addend;

  if (input_section)
    return input_section->output_section->shdr.sh_addr +
           input_section->offset + value;

  return value;
}

inline u64 StringPiece::get_addr() const {
  InputSection *is = isec;
  return is->merged_section->shdr.sh_addr + is->merged_offset + output_offset;
}

//
// object_file.cc
//

struct ComdatGroup {
  ComdatGroup(ObjectFile *file, u32 i)
    : file(file), section_idx(i) {}
  ComdatGroup(const ComdatGroup &other)
    : file(other.file.load()), section_idx(other.section_idx) {}

  tbb::spin_mutex mu;
  std::atomic<ObjectFile *> file;
  u32 section_idx;
};

class ObjectFile {
public:
  ObjectFile(MemoryBufferRef mb, StringRef archive_name);

  void parse();
  void initialize_mergeable_sections();
  void resolve_symbols();
  void mark_live_archive_members(tbb::parallel_do_feeder<ObjectFile *> &feeder);
  void hanlde_undefined_weak_symbols();
  void eliminate_duplicate_comdat_groups();
  void assign_mergeable_string_offsets();
  void convert_common_symbols();
  void scan_relocations();
  void compute_symtab();

  void write_local_symtab(u8 *buf, u64 symtab_off, u64 strtab_off);
  void write_global_symtab(u8 *buf, u64 symtab_off, u64 strtab_off);

  static ObjectFile *create_internal_file(ArrayRef<OutputChunk *> output_chunks);

  std::vector<InputSection *> sections;
  StringRef archive_name;
  ELFFile<ELF64LE> obj;
  std::vector<Symbol *> symbols;
  ArrayRef<ELF64LE::Sym> elf_syms;
  int first_global = 0;
  u32 priority;
  std::atomic_bool is_alive = ATOMIC_VAR_INIT(false);
  bool is_dso;
  const bool is_in_archive;
  std::string name;

  u64 local_symtab_size = 0;
  u64 local_strtab_size = 0;
  u64 global_symtab_size = 0;
  u64 global_strtab_size = 0;

  std::atomic_uint32_t num_plt = ATOMIC_VAR_INIT(0);
  std::atomic_uint32_t num_got = ATOMIC_VAR_INIT(0);
  std::atomic_uint32_t num_gotplt = ATOMIC_VAR_INIT(0);
  std::atomic_uint32_t num_relplt = ATOMIC_VAR_INIT(0);

  u32 got_offset = 0;
  u32 gotplt_offset = 0;
  u32 plt_offset = 0;
  u32 relplt_offset = 0;

  std::vector<InputSection *> mergeable_sections;

private:
  void initialize_sections();
  void initialize_symbols();
  void read_string_pieces(InputSection *isec);

  void maybe_override_symbol(const ELF64LE::Sym &esym, Symbol &sym, int idx);
  void remove_comdat_members(u32 section_idx);
  void write_symtab(u8 *buf, u64 symtab_off, u64 strtab_off, u32 start, u32 end);

  MemoryBufferRef mb;
  std::vector<std::pair<ComdatGroup *, u32>> comdat_groups;

  std::vector<Symbol> local_symbols;
  std::vector<StringPieceRef> sym_pieces;
  bool has_common_symbol;

  ArrayRef<ELF64LE::Shdr> elf_sections;
  StringRef symbol_strtab;
  const ELF64LE::Shdr *symtab_sec;
};

//
// perf.cc
//

class Counter {
public:
  Counter(StringRef name, u32 value = 0) : name(name), value(value) {
    static std::mutex mu;
    std::lock_guard lock(mu);
    instances.push_back(this);
  }

  void inc() {
    if (enabled)
      value++;
  }

  void inc(u32 delta) {
    if (enabled)
      value += delta;
  }

  void set(u32 value) {
    this->value = value;
  }

  static void print();

  static bool enabled;

private:
  StringRef name;
  std::atomic_uint32_t value;
  
  static std::vector<Counter *> instances;
};

//
// mapfile.cc
//

void print_map(ArrayRef<ObjectFile *> files, ArrayRef<OutputChunk *> output_sections);

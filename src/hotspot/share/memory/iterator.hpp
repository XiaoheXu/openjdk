/*
 * Copyright (c) 1997, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_MEMORY_ITERATOR_HPP
#define SHARE_VM_MEMORY_ITERATOR_HPP

#include "memory/allocation.hpp"
#include "memory/memRegion.hpp"
#include "oops/oopsHierarchy.hpp"

class CodeBlob;
class nmethod;
class ReferenceDiscoverer;
class DataLayout;
class KlassClosure;
class ClassLoaderData;
class Symbol;

// The following classes are C++ `closures` for iterating over objects, roots and spaces

class Closure : public StackObj { };

// OopClosure is used for iterating through references to Java objects.
class OopClosure : public Closure {
 public:
  virtual void do_oop(oop* o) = 0;
  virtual void do_oop(narrowOop* o) = 0;
};

class DoNothingClosure : public OopClosure {
 public:
  virtual void do_oop(oop* p)       {}
  virtual void do_oop(narrowOop* p) {}
};
extern DoNothingClosure do_nothing_cl;

// ExtendedOopClosure adds extra code to be run during oop iterations.
// This is needed by the GC and is extracted to a separate type to not
// pollute the OopClosure interface.
class ExtendedOopClosure : public OopClosure {
 private:
  ReferenceDiscoverer* _ref_discoverer;

 protected:
  ExtendedOopClosure(ReferenceDiscoverer* rd) : _ref_discoverer(rd) { }
  ExtendedOopClosure() : _ref_discoverer(NULL) { }
  ~ExtendedOopClosure() { }

  void set_ref_discoverer_internal(ReferenceDiscoverer* rd) { _ref_discoverer = rd; }

 public:
  ReferenceDiscoverer* ref_discoverer() const { return _ref_discoverer; }

  // Iteration of InstanceRefKlasses differ depending on the closure,
  // the below enum describes the different alternatives.
  enum ReferenceIterationMode {
    DO_DISCOVERY,                // Apply closure and discover references
    DO_DISCOVERED_AND_DISCOVERY, // Apply closure to discovered field and do discovery
    DO_FIELDS                    // Apply closure to all fields
  };

  // The default iteration mode is to do discovery.
  virtual ReferenceIterationMode reference_iteration_mode() { return DO_DISCOVERY; }

  // If the do_metadata functions return "true",
  // we invoke the following when running oop_iterate():
  //
  // 1) do_klass on the header klass pointer.
  // 2) do_klass on the klass pointer in the mirrors.
  // 3) do_cld   on the class loader data in class loaders.
  //
  // The virtual (without suffix) and the non-virtual (with _nv suffix) need
  // to be updated together, or else the devirtualization will break.
  //
  // Providing default implementations of the _nv functions unfortunately
  // removes the compile-time safeness, but reduces the clutter for the
  // ExtendedOopClosures that don't need to walk the metadata.
  // Currently, only CMS and G1 need these.

  bool do_metadata_nv()      { return false; }
  virtual bool do_metadata() { return do_metadata_nv(); }

  void do_klass_nv(Klass* k)      { ShouldNotReachHere(); }
  virtual void do_klass(Klass* k) { do_klass_nv(k); }

  void do_cld_nv(ClassLoaderData* cld)      { ShouldNotReachHere(); }
  virtual void do_cld(ClassLoaderData* cld) { do_cld_nv(cld); }

  // True iff this closure may be safely applied more than once to an oop
  // location without an intervening "major reset" (like the end of a GC).
  virtual bool idempotent() { return false; }

#ifdef ASSERT
  // Default verification of each visited oop field.
  template <typename T> void verify(T* p);

  // Can be used by subclasses to turn off the default verification of oop fields.
  virtual bool should_verify_oops() { return true; }
#endif
};

// Wrapper closure only used to implement oop_iterate_no_header().
class NoHeaderExtendedOopClosure : public ExtendedOopClosure {
  OopClosure* _wrapped_closure;
 public:
  NoHeaderExtendedOopClosure(OopClosure* cl) : _wrapped_closure(cl) {}
  // Warning: this calls the virtual version do_oop in the the wrapped closure.
  void do_oop_nv(oop* p)       { _wrapped_closure->do_oop(p); }
  void do_oop_nv(narrowOop* p) { _wrapped_closure->do_oop(p); }

  void do_oop(oop* p)          { assert(false, "Only the _nv versions should be used");
                                 _wrapped_closure->do_oop(p); }
  void do_oop(narrowOop* p)    { assert(false, "Only the _nv versions should be used");
                                 _wrapped_closure->do_oop(p);}
};

class KlassClosure : public Closure {
 public:
  virtual void do_klass(Klass* k) = 0;
};

class CLDClosure : public Closure {
 public:
  virtual void do_cld(ClassLoaderData* cld) = 0;
};


class CLDToOopClosure : public CLDClosure {
  OopClosure*       _oop_closure;
  bool              _must_claim_cld;

 public:
  CLDToOopClosure(OopClosure* oop_closure, bool must_claim_cld = true) :
      _oop_closure(oop_closure),
      _must_claim_cld(must_claim_cld) {}

  void do_cld(ClassLoaderData* cld);
};

// The base class for all concurrent marking closures,
// that participates in class unloading.
// It's used to proxy through the metadata to the oops defined in them.
class MetadataAwareOopClosure: public ExtendedOopClosure {

 public:
  MetadataAwareOopClosure() : ExtendedOopClosure() { }
  MetadataAwareOopClosure(ReferenceDiscoverer* rd) : ExtendedOopClosure(rd) { }

  bool do_metadata_nv()      { return true; }
  virtual bool do_metadata() { return do_metadata_nv(); }

  void do_klass_nv(Klass* k);
  virtual void do_klass(Klass* k) { do_klass_nv(k); }

  void do_cld_nv(ClassLoaderData* cld);
  virtual void do_cld(ClassLoaderData* cld) { do_cld_nv(cld); }
};

// ObjectClosure is used for iterating through an object space

class ObjectClosure : public Closure {
 public:
  // Called for each object.
  virtual void do_object(oop obj) = 0;
};


class BoolObjectClosure : public Closure {
 public:
  virtual bool do_object_b(oop obj) = 0;
};

class AlwaysTrueClosure: public BoolObjectClosure {
 public:
  bool do_object_b(oop p) { return true; }
};

class AlwaysFalseClosure : public BoolObjectClosure {
 public:
  bool do_object_b(oop p) { return false; }
};

// Applies an oop closure to all ref fields in objects iterated over in an
// object iteration.
class ObjectToOopClosure: public ObjectClosure {
  ExtendedOopClosure* _cl;
public:
  void do_object(oop obj);
  ObjectToOopClosure(ExtendedOopClosure* cl) : _cl(cl) {}
};

// A version of ObjectClosure that is expected to be robust
// in the face of possibly uninitialized objects.
class ObjectClosureCareful : public ObjectClosure {
 public:
  virtual size_t do_object_careful_m(oop p, MemRegion mr) = 0;
  virtual size_t do_object_careful(oop p) = 0;
};

// The following are used in CompactibleFreeListSpace and
// ConcurrentMarkSweepGeneration.

// Blk closure (abstract class)
class BlkClosure : public StackObj {
 public:
  virtual size_t do_blk(HeapWord* addr) = 0;
};

// A version of BlkClosure that is expected to be robust
// in the face of possibly uninitialized objects.
class BlkClosureCareful : public BlkClosure {
 public:
  size_t do_blk(HeapWord* addr) {
    guarantee(false, "call do_blk_careful instead");
    return 0;
  }
  virtual size_t do_blk_careful(HeapWord* addr) = 0;
};

// SpaceClosure is used for iterating over spaces

class Space;
class CompactibleSpace;

class SpaceClosure : public StackObj {
 public:
  // Called for each space
  virtual void do_space(Space* s) = 0;
};

class CompactibleSpaceClosure : public StackObj {
 public:
  // Called for each compactible space
  virtual void do_space(CompactibleSpace* s) = 0;
};


// CodeBlobClosure is used for iterating through code blobs
// in the code cache or on thread stacks

class CodeBlobClosure : public Closure {
 public:
  // Called for each code blob.
  virtual void do_code_blob(CodeBlob* cb) = 0;
};

// Applies an oop closure to all ref fields in code blobs
// iterated over in an object iteration.
class CodeBlobToOopClosure : public CodeBlobClosure {
  OopClosure* _cl;
  bool _fix_relocations;
 protected:
  void do_nmethod(nmethod* nm);
 public:
  // If fix_relocations(), then cl must copy objects to their new location immediately to avoid
  // patching nmethods with the old locations.
  CodeBlobToOopClosure(OopClosure* cl, bool fix_relocations) : _cl(cl), _fix_relocations(fix_relocations) {}
  virtual void do_code_blob(CodeBlob* cb);

  bool fix_relocations() const { return _fix_relocations; }
  const static bool FixRelocations = true;
};

class MarkingCodeBlobClosure : public CodeBlobToOopClosure {
 public:
  MarkingCodeBlobClosure(OopClosure* cl, bool fix_relocations) : CodeBlobToOopClosure(cl, fix_relocations) {}
  // Called for each code blob, but at most once per unique blob.

  virtual void do_code_blob(CodeBlob* cb);
};

// MonitorClosure is used for iterating over monitors in the monitors cache

class ObjectMonitor;

class MonitorClosure : public StackObj {
 public:
  // called for each monitor in cache
  virtual void do_monitor(ObjectMonitor* m) = 0;
};

// A closure that is applied without any arguments.
class VoidClosure : public StackObj {
 public:
  // I would have liked to declare this a pure virtual, but that breaks
  // in mysterious ways, for unknown reasons.
  virtual void do_void();
};


// YieldClosure is intended for use by iteration loops
// to incrementalize their work, allowing interleaving
// of an interruptable task so as to allow other
// threads to run (which may not otherwise be able to access
// exclusive resources, for instance). Additionally, the
// closure also allows for aborting an ongoing iteration
// by means of checking the return value from the polling
// call.
class YieldClosure : public StackObj {
public:
 virtual bool should_return() = 0;

 // Yield on a fine-grain level. The check in case of not yielding should be very fast.
 virtual bool should_return_fine_grain() { return false; }
};

// Abstract closure for serializing data (read or write).

class SerializeClosure : public Closure {
public:
  // Return bool indicating whether closure implements read or write.
  virtual bool reading() const = 0;

  // Read/write the void pointer pointed to by p.
  virtual void do_ptr(void** p) = 0;

  // Read/write the 32-bit unsigned integer pointed to by p.
  virtual void do_u4(u4* p) = 0;

  // Read/write the region specified.
  virtual void do_region(u_char* start, size_t size) = 0;

  // Check/write the tag.  If reading, then compare the tag against
  // the passed in value and fail is they don't match.  This allows
  // for verification that sections of the serialized data are of the
  // correct length.
  virtual void do_tag(int tag) = 0;

  // Read/write the oop
  virtual void do_oop(oop* o) = 0;

  bool writing() {
    return !reading();
  }
};

class SymbolClosure : public StackObj {
 public:
  virtual void do_symbol(Symbol**) = 0;

  // Clear LSB in symbol address; it can be set by CPSlot.
  static Symbol* load_symbol(Symbol** p) {
    return (Symbol*)(intptr_t(*p) & ~1);
  }

  // Store symbol, adjusting new pointer if the original pointer was adjusted
  // (symbol references in constant pool slots have their LSB set to 1).
  static void store_symbol(Symbol** p, Symbol* sym) {
    *p = (Symbol*)(intptr_t(sym) | (intptr_t(*p) & 1));
  }
};

// The two class template specializations are used to dispatch calls
// to the ExtendedOopClosure functions. If use_non_virtual_call is true,
// the non-virtual versions are called (E.g. do_oop_nv), otherwise the
// virtual versions are called (E.g. do_oop).

template <bool use_non_virtual_call>
class Devirtualizer {};

// Dispatches to the non-virtual functions.
template <> class Devirtualizer<true> {
 public:
  template <class OopClosureType, typename T> static void do_oop(OopClosureType* closure, T* p);
  template <class OopClosureType>             static void do_klass(OopClosureType* closure, Klass* k);
  template <class OopClosureType>             static void do_cld(OopClosureType* closure, ClassLoaderData* cld);
  template <class OopClosureType>             static bool do_metadata(OopClosureType* closure);
};

// Dispatches to the virtual functions.
template <> class Devirtualizer<false> {
 public:
  template <class OopClosureType, typename T> static void do_oop(OopClosureType* closure, T* p);
  template <class OopClosureType>             static void do_klass(OopClosureType* closure, Klass* k);
  template <class OopClosureType>             static void do_cld(OopClosureType* closure, ClassLoaderData* cld);
  template <class OopClosureType>             static bool do_metadata(OopClosureType* closure);
};

#endif // SHARE_VM_MEMORY_ITERATOR_HPP

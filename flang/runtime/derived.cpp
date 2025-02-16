//===-- runtime/derived.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "derived.h"
#include "stat.h"
#include "terminator.h"
#include "type-info.h"
#include "flang/Runtime/descriptor.h"

namespace Fortran::runtime {

int Initialize(const Descriptor &instance, const typeInfo::DerivedType &derived,
    Terminator &terminator, bool hasStat, const Descriptor *errMsg) {
  const Descriptor &componentDesc{derived.component()};
  std::size_t elements{instance.Elements()};
  std::size_t byteStride{instance.ElementBytes()};
  int stat{StatOk};
  // Initialize data components in each element; the per-element iterations
  // constitute the inner loops, not the outer ones
  std::size_t myComponents{componentDesc.Elements()};
  for (std::size_t k{0}; k < myComponents; ++k) {
    const auto &comp{
        *componentDesc.ZeroBasedIndexedElement<typeInfo::Component>(k)};
    if (comp.genre() == typeInfo::Component::Genre::Allocatable ||
        comp.genre() == typeInfo::Component::Genre::Automatic) {
      for (std::size_t j{0}; j < elements; ++j) {
        Descriptor &allocDesc{*instance.OffsetElement<Descriptor>(
            j * byteStride + comp.offset())};
        comp.EstablishDescriptor(allocDesc, instance, terminator);
        allocDesc.raw().attribute = CFI_attribute_allocatable;
        if (comp.genre() == typeInfo::Component::Genre::Automatic) {
          stat = ReturnError(terminator, allocDesc.Allocate(), errMsg, hasStat);
          if (stat == StatOk) {
            if (const DescriptorAddendum * addendum{allocDesc.Addendum()}) {
              if (const auto *derived{addendum->derivedType()}) {
                if (!derived->noInitializationNeeded()) {
                  stat = Initialize(
                      allocDesc, *derived, terminator, hasStat, errMsg);
                }
              }
            }
          }
          if (stat != StatOk) {
            break;
          }
        }
      }
    } else if (const void *init{comp.initialization()}) {
      // Explicit initialization of data pointers and
      // non-allocatable non-automatic components
      std::size_t bytes{comp.SizeInBytes(instance)};
      for (std::size_t j{0}; j < elements; ++j) {
        char *ptr{instance.ZeroBasedIndexedElement<char>(j) + comp.offset()};
        std::memcpy(ptr, init, bytes);
      }
    } else if (comp.genre() == typeInfo::Component::Genre::Pointer) {
      // Data pointers without explicit initialization are established
      // so that they are valid right-hand side targets of pointer
      // assignment statements.
      for (std::size_t j{0}; j < elements; ++j) {
        Descriptor &ptrDesc{*instance.OffsetElement<Descriptor>(
            j * byteStride + comp.offset())};
        comp.EstablishDescriptor(ptrDesc, instance, terminator);
        ptrDesc.raw().attribute = CFI_attribute_pointer;
      }
    } else if (comp.genre() == typeInfo::Component::Genre::Data &&
        comp.derivedType() && !comp.derivedType()->noInitializationNeeded()) {
      // Default initialization of non-pointer non-allocatable/automatic
      // data component.  Handles parent component's elements.  Recursive.
      SubscriptValue extent[maxRank];
      const typeInfo::Value *bounds{comp.bounds()};
      for (int dim{0}; dim < comp.rank(); ++dim) {
        typeInfo::TypeParameterValue lb{
            bounds[2 * dim].GetValue(&instance).value_or(0)};
        typeInfo::TypeParameterValue ub{
            bounds[2 * dim + 1].GetValue(&instance).value_or(0)};
        extent[dim] = ub >= lb ? ub - lb + 1 : 0;
      }
      StaticDescriptor<maxRank, true, 0> staticDescriptor;
      Descriptor &compDesc{staticDescriptor.descriptor()};
      const typeInfo::DerivedType &compType{*comp.derivedType()};
      for (std::size_t j{0}; j < elements; ++j) {
        compDesc.Establish(compType,
            instance.OffsetElement<char>(j * byteStride + comp.offset()),
            comp.rank(), extent);
        stat = Initialize(compDesc, compType, terminator, hasStat, errMsg);
        if (stat != StatOk) {
          break;
        }
      }
    }
  }
  // Initialize procedure pointer components in each element
  const Descriptor &procPtrDesc{derived.procPtr()};
  std::size_t myProcPtrs{procPtrDesc.Elements()};
  for (std::size_t k{0}; k < myProcPtrs; ++k) {
    const auto &comp{
        *procPtrDesc.ZeroBasedIndexedElement<typeInfo::ProcPtrComponent>(k)};
    for (std::size_t j{0}; j < elements; ++j) {
      auto &pptr{*instance.OffsetElement<typeInfo::ProcedurePointer>(
          j * byteStride + comp.offset)};
      pptr = comp.procInitialization;
    }
  }
  return stat;
}

static const typeInfo::SpecialBinding *FindFinal(
    const typeInfo::DerivedType &derived, int rank) {
  if (const auto *ranked{derived.FindSpecialBinding(
          typeInfo::SpecialBinding::RankFinal(rank))}) {
    return ranked;
  } else if (const auto *assumed{derived.FindSpecialBinding(
                 typeInfo::SpecialBinding::Which::AssumedRankFinal)}) {
    return assumed;
  } else {
    return derived.FindSpecialBinding(
        typeInfo::SpecialBinding::Which::ElementalFinal);
  }
}

static void CallFinalSubroutine(
    const Descriptor &descriptor, const typeInfo::DerivedType &derived) {
  if (const auto *special{FindFinal(derived, descriptor.rank())}) {
    // The following code relies on the fact that finalizable objects
    // must be contiguous.
    if (special->which() == typeInfo::SpecialBinding::Which::ElementalFinal) {
      std::size_t byteStride{descriptor.ElementBytes()};
      std::size_t elements{descriptor.Elements()};
      if (special->IsArgDescriptor(0)) {
        StaticDescriptor<maxRank, true, 8 /*?*/> statDesc;
        Descriptor &elemDesc{statDesc.descriptor()};
        elemDesc = descriptor;
        elemDesc.raw().attribute = CFI_attribute_pointer;
        elemDesc.raw().rank = 0;
        auto *p{special->GetProc<void (*)(const Descriptor &)>()};
        for (std::size_t j{0}; j < elements; ++j) {
          elemDesc.set_base_addr(
              descriptor.OffsetElement<char>(j * byteStride));
          p(elemDesc);
        }
      } else {
        auto *p{special->GetProc<void (*)(char *)>()};
        for (std::size_t j{0}; j < elements; ++j) {
          p(descriptor.OffsetElement<char>(j * byteStride));
        }
      }
    } else if (special->IsArgDescriptor(0)) {
      StaticDescriptor<maxRank, true, 8 /*?*/> statDesc;
      Descriptor &tmpDesc{statDesc.descriptor()};
      tmpDesc = descriptor;
      tmpDesc.raw().attribute = CFI_attribute_pointer;
      tmpDesc.Addendum()->set_derivedType(&derived);
      auto *p{special->GetProc<void (*)(const Descriptor &)>()};
      p(tmpDesc);
    } else {
      auto *p{special->GetProc<void (*)(char *)>()};
      p(descriptor.OffsetElement<char>());
    }
  }
}

// Fortran 2018 subclause 7.5.6.2
void Finalize(
    const Descriptor &descriptor, const typeInfo::DerivedType &derived) {
  if (derived.noFinalizationNeeded() || !descriptor.IsAllocated()) {
    return;
  }
  CallFinalSubroutine(descriptor, derived);
  const auto *parentType{derived.GetParentType()};
  bool recurse{parentType && !parentType->noFinalizationNeeded()};
  // If there's a finalizable parent component, handle it last, as required
  // by the Fortran standard (7.5.6.2), and do so recursively with the same
  // descriptor so that the rank is preserved.
  const Descriptor &componentDesc{derived.component()};
  std::size_t myComponents{componentDesc.Elements()};
  std::size_t elements{descriptor.Elements()};
  std::size_t byteStride{descriptor.ElementBytes()};
  for (auto k{recurse
               ? std::size_t{1} /* skip first component, it's the parent */
               : 0};
       k < myComponents; ++k) {
    const auto &comp{
        *componentDesc.ZeroBasedIndexedElement<typeInfo::Component>(k)};
    if (comp.genre() == typeInfo::Component::Genre::Allocatable ||
        comp.genre() == typeInfo::Component::Genre::Automatic) {
      if (const typeInfo::DerivedType * compType{comp.derivedType()}) {
        if (!compType->noFinalizationNeeded()) {
          for (std::size_t j{0}; j < elements; ++j) {
            const Descriptor &compDesc{*descriptor.OffsetElement<Descriptor>(
                j * byteStride + comp.offset())};
            if (compDesc.IsAllocated()) {
              Finalize(compDesc, *compType);
            }
          }
        }
      }
    } else if (comp.genre() == typeInfo::Component::Genre::Data &&
        comp.derivedType() && !comp.derivedType()->noFinalizationNeeded()) {
      SubscriptValue extent[maxRank];
      const typeInfo::Value *bounds{comp.bounds()};
      for (int dim{0}; dim < comp.rank(); ++dim) {
        SubscriptValue lb{bounds[2 * dim].GetValue(&descriptor).value_or(0)};
        SubscriptValue ub{
            bounds[2 * dim + 1].GetValue(&descriptor).value_or(0)};
        extent[dim] = ub >= lb ? ub - lb + 1 : 0;
      }
      StaticDescriptor<maxRank, true, 0> staticDescriptor;
      Descriptor &compDesc{staticDescriptor.descriptor()};
      const typeInfo::DerivedType &compType{*comp.derivedType()};
      for (std::size_t j{0}; j < elements; ++j) {
        compDesc.Establish(compType,
            descriptor.OffsetElement<char>(j * byteStride + comp.offset()),
            comp.rank(), extent);
        Finalize(compDesc, compType);
      }
    }
  }
  if (recurse) {
    Finalize(descriptor, *parentType);
  }
}

// The order of finalization follows Fortran 2018 7.5.6.2, with
// elementwise finalization of non-parent components taking place
// before parent component finalization, and with all finalization
// preceding any deallocation.
void Destroy(const Descriptor &descriptor, bool finalize,
    const typeInfo::DerivedType &derived) {
  if (derived.noDestructionNeeded() || !descriptor.IsAllocated()) {
    return;
  }
  if (finalize && !derived.noFinalizationNeeded()) {
    Finalize(descriptor, derived);
  }
  const Descriptor &componentDesc{derived.component()};
  std::size_t myComponents{componentDesc.Elements()};
  std::size_t elements{descriptor.Elements()};
  SubscriptValue at[maxRank];
  descriptor.GetLowerBounds(at);
  for (std::size_t k{0}; k < myComponents; ++k) {
    const auto &comp{
        *componentDesc.ZeroBasedIndexedElement<typeInfo::Component>(k)};
    if (comp.genre() == typeInfo::Component::Genre::Allocatable ||
        comp.genre() == typeInfo::Component::Genre::Automatic) {
      for (std::size_t j{0}; j < elements; ++j) {
        Descriptor *d{reinterpret_cast<Descriptor *>(
            descriptor.Element<char>(at) + comp.offset())};
        d->Deallocate();
        descriptor.IncrementSubscripts(at);
      }
    }
  }
}

bool HasDynamicComponent(const Descriptor &descriptor) {
  if (const DescriptorAddendum * addendum{descriptor.Addendum()}) {
    if (const auto *derived = addendum->derivedType()) {
      const Descriptor &componentDesc{derived->component()};
      std::size_t myComponents{componentDesc.Elements()};
      for (std::size_t k{0}; k < myComponents; ++k) {
        const auto &comp{
            *componentDesc.ZeroBasedIndexedElement<typeInfo::Component>(k)};
        if (comp.genre() == typeInfo::Component::Genre::Allocatable ||
            comp.genre() == typeInfo::Component::Genre::Automatic) {
          return true;
        }
      }
    }
  }
  return false;
}

} // namespace Fortran::runtime

//===- OutputSections.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OutputSections.h"
#include "InputChunks.h"
#include "InputFiles.h"
#include "OutputSegment.h"
#include "WriterUtils.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/Parallel.h"

#define DEBUG_TYPE "lld"

using namespace llvm;
using namespace llvm::wasm;

namespace lld {

// Returns a string, e.g. "FUNCTION(.text)".
std::string toString(const wasm::OutputSection &sec) {
  if (!sec.name.empty())
    return (sec.getSectionName() + "(" + sec.name + ")").str();
  return std::string(sec.getSectionName());
}

namespace wasm {
static StringRef sectionTypeToString(uint32_t sectionType) {
  switch (sectionType) {
  case WASM_SEC_CUSTOM:
    return "CUSTOM";
  case WASM_SEC_TYPE:
    return "TYPE";
  case WASM_SEC_IMPORT:
    return "IMPORT";
  case WASM_SEC_FUNCTION:
    return "FUNCTION";
  case WASM_SEC_TABLE:
    return "TABLE";
  case WASM_SEC_MEMORY:
    return "MEMORY";
  case WASM_SEC_GLOBAL:
    return "GLOBAL";
  case WASM_SEC_EVENT:
    return "EVENT";
  case WASM_SEC_EXPORT:
    return "EXPORT";
  case WASM_SEC_START:
    return "START";
  case WASM_SEC_ELEM:
    return "ELEM";
  case WASM_SEC_CODE:
    return "CODE";
  case WASM_SEC_DATA:
    return "DATA";
  case WASM_SEC_DATACOUNT:
    return "DATACOUNT";
  default:
    fatal("invalid section type");
  }
}

StringRef OutputSection::getSectionName() const {
  return sectionTypeToString(type);
}

void OutputSection::createHeader(size_t bodySize) {
  raw_string_ostream os(header);
  debugWrite(os.tell(), "section type [" + getSectionName() + "]");
  encodeULEB128(type, os);
  writeUleb128(os, bodySize, "section size");
  os.flush();
  log("createHeader: " + toString(*this) + " body=" + Twine(bodySize) +
      " total=" + Twine(getSize()));
}

void CodeSection::finalizeContents() {
  raw_string_ostream os(codeSectionHeader);
  writeUleb128(os, functions.size(), "function count");
  os.flush();
  bodySize = codeSectionHeader.size();

  for (InputFunction *func : functions) {
    func->outputSec = this;
    func->outSecOff = bodySize;
    func->calculateSize();
    // All functions should have a non-empty body at this point
    assert(func->getSize());
    bodySize += func->getSize();
  }

  createHeader(bodySize);
}

void CodeSection::writeTo(uint8_t *buf) {
  log("writing " + toString(*this));
  log(" size=" + Twine(getSize()));
  log(" headersize=" + Twine(header.size()));
  log(" codeheadersize=" + Twine(codeSectionHeader.size()));
  buf += offset;

  // Write section header
  memcpy(buf, header.data(), header.size());
  buf += header.size();

  // Write code section headers
  memcpy(buf, codeSectionHeader.data(), codeSectionHeader.size());

  // Write code section bodies
  for (const InputChunk *chunk : functions)
    chunk->writeTo(buf);
}

uint32_t CodeSection::getNumRelocations() const {
  uint32_t count = 0;
  for (const InputChunk *func : functions)
    count += func->getNumRelocations();
  return count;
}

void CodeSection::writeRelocations(raw_ostream &os) const {
  for (const InputChunk *c : functions)
    c->writeRelocations(os);
}

void DataSection::finalizeContents() {
  raw_string_ostream os(dataSectionHeader);
  unsigned segmentCount =
      std::count_if(segments.begin(), segments.end(),
                    [](OutputSegment *segment) { return !segment->isBss; });

#ifndef NDEBUG
  unsigned activeCount = std::count_if(
      segments.begin(), segments.end(), [](OutputSegment *segment) {
        return (segment->initFlags & WASM_DATA_SEGMENT_IS_PASSIVE) == 0;
      });
#endif

  assert((!config->isPic || activeCount <= 1) &&
         "Currenly only a single data segment is supported in PIC mode");

  writeUleb128(os, segmentCount, "data segment count");
  os.flush();
  bodySize = dataSectionHeader.size();

  for (OutputSegment *segment : segments) {
    if (segment->isBss)
      continue;
    raw_string_ostream os(segment->header);
    writeUleb128(os, segment->initFlags, "init flags");
    if (segment->initFlags & WASM_DATA_SEGMENT_HAS_MEMINDEX)
      writeUleb128(os, 0, "memory index");
    if ((segment->initFlags & WASM_DATA_SEGMENT_IS_PASSIVE) == 0) {
      WasmInitExpr initExpr;
      if (config->isPic) {
        initExpr.Opcode = WASM_OPCODE_GLOBAL_GET;
        initExpr.Value.Global = WasmSym::memoryBase->getGlobalIndex();
      } else if (config->is64.getValueOr(false)) {
        initExpr.Opcode = WASM_OPCODE_I64_CONST;
        initExpr.Value.Int64 = static_cast<int64_t>(segment->startVA);
      } else {
        initExpr.Opcode = WASM_OPCODE_I32_CONST;
        initExpr.Value.Int32 = static_cast<int32_t>(segment->startVA);      
      }
      writeInitExpr(os, initExpr);
    }
    writeUleb128(os, segment->size, "segment size");
    os.flush();

    segment->sectionOffset = bodySize;
    bodySize += segment->header.size() + segment->size;
    log("Data segment: size=" + Twine(segment->size) + ", startVA=" +
        Twine::utohexstr(segment->startVA) + ", name=" + segment->name);

    for (InputChunk *inputSeg : segment->inputSegments) {
      inputSeg->outputSec = this;
      inputSeg->outSecOff = segment->sectionOffset + segment->header.size() +
                            inputSeg->outputSegmentOffset;
    }
  }

  createHeader(bodySize);
}

void DataSection::writeTo(uint8_t *buf) {
  log("writing " + toString(*this) + " size=" + Twine(getSize()) +
      " body=" + Twine(bodySize));
  buf += offset;

  // Write section header
  memcpy(buf, header.data(), header.size());
  buf += header.size();

  // Write data section headers
  memcpy(buf, dataSectionHeader.data(), dataSectionHeader.size());

  for (const OutputSegment *segment : segments) {
    if (segment->isBss)
      continue;
    // Write data segment header
    uint8_t *segStart = buf + segment->sectionOffset;
    memcpy(segStart, segment->header.data(), segment->header.size());

    // Write segment data payload
    for (const InputChunk *chunk : segment->inputSegments)
      chunk->writeTo(buf);
  }
}

uint32_t DataSection::getNumRelocations() const {
  uint32_t count = 0;
  for (const OutputSegment *seg : segments)
    for (const InputChunk *inputSeg : seg->inputSegments)
      count += inputSeg->getNumRelocations();
  return count;
}

void DataSection::writeRelocations(raw_ostream &os) const {
  for (const OutputSegment *seg : segments)
    for (const InputChunk *c : seg->inputSegments)
      c->writeRelocations(os);
}

bool DataSection::isNeeded() const {
  for (const OutputSegment *seg : segments)
    if (!seg->isBss)
      return true;
  return false;
}

// Lots of duplication here with OutputSegment::finalizeInputSegments
void CustomSection::finalizeInputSections() {
  SyntheticMergedChunk *mergedSection = nullptr;
  std::vector<InputChunk *> newSections;

  for (InputChunk *s : inputSections) {
    MergeInputChunk *ms = dyn_cast<MergeInputChunk>(s);
    if (!ms) {
      newSections.push_back(s);
      continue;
    }

    if (!mergedSection) {
      mergedSection =
          make<SyntheticMergedChunk>(name, 0, WASM_SEG_FLAG_STRINGS);
      newSections.push_back(mergedSection);
    }
    mergedSection->addMergeChunk(ms);
  }

  if (!mergedSection)
    return;

  mergedSection->finalizeContents();
  inputSections = newSections;
}

void CustomSection::finalizeContents() {
  finalizeInputSections();

  raw_string_ostream os(nameData);
  encodeULEB128(name.size(), os);
  os << name;
  os.flush();

  for (InputChunk *section : inputSections) {
    assert(!section->discarded);
    section->outputSec = this;
    section->outSecOff = payloadSize;
    payloadSize += section->getSize();
  }

  createHeader(payloadSize + nameData.size());
}

void CustomSection::writeTo(uint8_t *buf) {
  log("writing " + toString(*this) + " size=" + Twine(getSize()) +
      " chunks=" + Twine(inputSections.size()));

  assert(offset);
  buf += offset;

  // Write section header
  memcpy(buf, header.data(), header.size());
  buf += header.size();
  memcpy(buf, nameData.data(), nameData.size());
  buf += nameData.size();

  // Write custom sections payload
  for (const InputChunk *section : inputSections)
    section->writeTo(buf);
}

uint32_t CustomSection::getNumRelocations() const {
  uint32_t count = 0;
  for (const InputChunk *inputSect : inputSections)
    count += inputSect->getNumRelocations();
  return count;
}

void CustomSection::writeRelocations(raw_ostream &os) const {
  for (const InputChunk *s : inputSections)
    s->writeRelocations(os);
}

} // namespace wasm
} // namespace lld

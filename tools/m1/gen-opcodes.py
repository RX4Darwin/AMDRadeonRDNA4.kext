#!/usr/bin/env python3
"""Generate src/pvopcodes.inc: the name/size tables of the paravirtual GPU's three command layers (docs/m1-stream.md).

  stream      commands the Metal bundle's PGSerializer encoders write into command buffers (8-byte header {u32 id, u32 size} + payload)
  operation   commands PGSerializer writes into the 4 KiB 'operation' buffer (object creation / deletion), same header
  fifo        commands of the kernel driver's FIFO (12-byte header {u16 id, u16 barriers, u32 length, u32 signal})
The first two are extracted from Apple's AppleParavirtGPUMetal x86_64 slice by tools/m1/apvcmds.py (command id and payload length are the immediates of the
getCommandBytes:forCommand: / allocateOperationBytes: calls); a few ids that reach the writer through a helper's `withCommand:` argument are listed below
by hand (found with the same tools). The fifo ids come from the call sites of AppleParavirtCommandAllocator::init in the kext (tools/m0/pvdis.py); the
names are the calling functions', not Apple's enumerators. No Apple bytes are written, only numbers and our own names.

  gen-opcodes.py > src/pvopcodes.inc
  gen-opcodes.py --md > docs/m1-opcodes.md        (the same tables with the store layouts the scanner saw, for people)"""
import collections
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

GROUPS = {'PGSerializerRenderCommandEncoder': 'Render', 'PGSerializerComputeCommandEncoder': 'Compute', 'PGSerializerBlitCommandEncoder': 'Blit',
          'PGSerializerInfoCommandEncoder': 'Info', 'PGSerializerCommandEncoder': 'Common', 'PGSerializerParallelRenderCommandEncoder': 'ParallelRender',
          'PGSerializer': 'Op'}

# (id, payload length or -1, name): reach the writer through setBuffers/setTextures/setSamplerStates/setBytes/optimize helpers' withCommand: argument
HELPER_IDS = [
    (0x6e, -1, 'Render.setFragmentBuffers'), (0x70, -1, 'Render.setFragmentSamplerStates'), (0x71, -1, 'Render.setFragmentSamplerStatesLod'),
    (0x72, -1, 'Render.setFragmentTextures'), (0x7d, -1, 'Render.setVertexBuffers'), (0x7f, -1, 'Render.setVertexSamplerStates'),
    (0x80, -1, 'Render.setVertexSamplerStatesLod'), (0x81, -1, 'Render.setVertexTextures'), (0x9d, -1, 'Render.setTileBuffers'),
    (0x9f, -1, 'Render.setTileSamplerStates'), (0xa0, -1, 'Render.setTileSamplerStatesLod'), (0xa1, -1, 'Render.setTileTextures'),
    (0xa5, -1, 'Render.setVertexBuffersAttributeStride'),
    (0xcb, -1, 'Compute.setBuffers'), (0xcc, -1, 'Compute.setSamplerStates'), (0xcd, -1, 'Compute.setSamplerStatesLod'), (0xce, -1, 'Compute.setTextures'),
    (0xd9, -1, 'Compute.setBuffersAttributeStride'),
    (0x134, 4, 'Blit.optimizeContentsForCPUAccess'), (0x135, 4, 'Blit.optimizeContentsForGPUAccess'),
    (0x136, 8, 'Blit.optimizeContentsForCPUAccessSliceLevel'), (0x137, 8, 'Blit.optimizeContentsForGPUAccessSliceLevel'),
    (0x138, 20, 'Blit.optimizeIndirectCommandBuffer'), (0x139, 20, 'Blit.resetCommandsInBuffer'),
]

# kernel driver FIFO command ids -> (name, calling function): tools/m0/pvdis.py xref on AppleParavirtCommandAllocator::init
FIFO = [
    (0x01, 'DisplaySetupSharedState', 'AppleParavirtDisplayPipe::setupSharedState'),
    (0x02, 'DisplayProcessOnline', 'AppleParavirtDisplayPipe::process_online'),
    (0x04, 'DisplayUpdateCursorGlyph', 'AppleParavirtDisplayPipe::updateCursorGlyph'),
    (0x05, 'DisplayUpdateCursorState', 'AppleParavirtDisplayPipe::updateCursorState'),
    (0x06, 'DisplaySubmitTransaction', 'AppleParavirtDisplayPipe::submitTransaction'),
    (0x07, 'DisplaySubmitTransaction2', 'AppleParavirtDisplayPipe::submitTransaction'),
    (0x1e, 'DisplayFlushChannelEvent', 'AppleParavirtDisplayPipe::flushChannelEvent'),
    (0x20, 'FreeTask', 'AppleParavirtTask::free'),
    (0x22, 'ReleaseFromGPUPageTable', 'AppleParavirtMemoryMap::releaseFromGPUPageTable'),
    (0x25, 'DeleteHostResourceID', 'AppleParavirtResource::deleteHostResourceID'),
    (0x28, 'DeleteObject', 'AppleParavirtShared::deleteObject'),
    (0x30, 'DefineChannel', 'AppleParavirtVirtualChannel::init'),
    (0x31, 'FreeChannel', 'AppleParavirtVirtualChannel::free'),
    (0x33, 'SetResourceHeap', 'AppleParavirtTask::setResourceHeap'),
    (0x34, 'PageBacking', 'AppleParavirtResource::pageBacking'),
    (0x35, 'SynchronizeForUnwire', 'AppleParavirtResource::synchronizeForUnwire'),
    (0x36, 'DeleteHostIOSurfaceBacking', 'AppleParavirtResource::deleteHostIOSurfaceBacking'),
    (0x37, 'ExecIndirect', 'AppleParavirtCommandQueue::processExecIndirect'),
    (0x38, 'DefineHostTask', 'AppleParavirtTask::defineHostTask'),
    (0x39, 'CommitIntoGPUPageTable', 'AppleParavirtMemoryMap::commitIntoGPUPageTable'),
    (0x3a, 'GetDeviceInfo', 'AppleParavirtAccelerator::setupDeviceInfo'),
    (0x3b, 'CreateComputePipeline', 'AppleParavirtShared::createComputePipeline'),
    (0x3c, 'ReplacePhysical', 'AppleParavirtResource::replacePhysical'),
    (0x41, 'DeleteHostSharedTextureBacking', 'AppleParavirtResource::deleteHostSharedTextureBacking'),
]


def short(method):
    m = re.match(r'[-+]\[(\w+) ([^\]]+)\]', method)
    cls, sel = m.group(1), m.group(2)
    return GROUPS.get(cls, cls) + '.' + sel.split(':')[0]


def clean(sd):
    sd = re.sub(r'^result of -', '', sd)
    sd = re.sub(r'^arg\d+\((.*)\)$', r'\1', sd)
    return sd


def markdown(rows):
    out = ['# Command tables of the paravirtual GPU stream (generated)', '',
           'Generated by `tools/m1/gen-opcodes.py --md` from Apple\'s `AppleParavirtGPUMetal` x86_64 slice (`tools/m1/apvcmds.py`). **Measured**: ids and payload lengths',
           '(immediates at the writers\' call sites). **Reading aid, not a decompiler**: the field lists are the stores the scanner saw into the returned buffer',
           '(offset from the payload start, width, source); a source is the Objective-C accessor whose result is stored (`bufferRef` is the PGSerializer\'s u32 object',
           'reference of a buffer, `parentResourceOffset` the byte offset of a sub-allocated buffer inside its resource), the method\'s own argument (named by',
           'its selector part) or a register the scanner could not trace. Struct arguments passed by value (`MTLSize`, `MTLOrigin`, `MTLRegion`) appear as',
           '16-byte stores and 8-byte tails. Sizes: a command occupies `align4(payload + 8)` bytes in the stream.', '']
    groups = collections.OrderedDict((g, []) for g in ('Render', 'Compute', 'Blit', 'Info', 'Common', 'ParallelRender', 'Op'))
    seen = set()
    for r in sorted((x for x in rows if x['cmd'] is not None), key=lambda x: (x['kind'], x['cmd'], x['method'])):
        g = GROUPS.get(re.match(r'[-+]\[(\w+) ', r['method']).group(1), 'Other')
        key = (r['kind'], r['cmd'], r['method'].split()[1], r['len'])
        if key in seen:
            continue
        seen.add(key)
        fields = ', '.join('+0x%02x u%d %s' % (o, w * 8, clean(sd)) for o, w, sd in r['stores'])
        ln = r['len']
        if r['kind'] == 'operation' and ln is not None and ln >= 8:
            ln -= 8
        groups.setdefault(g if r['kind'] == 'stream' else 'Op', []).append((r['kind'], r['cmd'], ln, r['method'], fields))
    for g, items in groups.items():
        if not items:
            continue
        kind = items[0][0]
        out += ['## %s %s' % (g, 'commands (stream)' if kind == 'stream' else '"operations" (object creation and deletion buffer)'), '',
                '| id | payload | method | fields |', '|---|---|---|---|']
        for kind, cid, ln, method, fields in items:
            out.append('| 0x%03x | %s | `%s` | %s |' % (cid, '?' if ln is None else ln, re.sub(r'^[-+]\[\w+ ', '', method).rstrip(']'), fields))
        out.append('')
    out += ['## Commands that reach the writer through a helper\'s `withCommand:` argument', '',
            'Variable-length binding commands (`setBuffers:offsets:withRange:withCommand:bindings:` and its siblings write the range and the per-slot entries); ids found at the',
            'callers: ' + ', '.join('0x%x %s' % (i, n) for i, l, n in HELPER_IDS), '']
    return '\n'.join(out)


def main():
    rows = json.loads(subprocess.run([sys.executable, os.path.join(HERE, 'apvcmds.py'), 'json'], capture_output=True, text=True, check=True).stdout)
    if '--md' in sys.argv:
        print(markdown(rows))
        return
    tables = {'stream': collections.OrderedDict(), 'operation': collections.OrderedDict()}
    for r in rows:
        if r['cmd'] is None or r['kind'] not in tables:
            continue
        t = tables[r['kind']]
        name = short(r['method'])
        ln = r['len'] if r['len'] is not None else -1
        if r['kind'] == 'operation' and ln >= 8:
            ln -= 8        # operation headers carry the total size, payload excludes the 8-byte header
        e = t.setdefault(r['cmd'], {'names': [], 'lens': set()})
        if name not in e['names']:
            e['names'].append(name)
        e['lens'].add(ln)
    for cid, ln, name in HELPER_IDS:
        e = tables['stream'].setdefault(cid, {'names': [], 'lens': set()})
        e['names'].append(name)
        e['lens'].add(ln)
    out = ['// Generated by tools/m1/gen-opcodes.py (docs/m1-stream.md). Do not edit. Names are ours, derived from the selectors of Apple\'s PGSerializer and',
           '// from the kernel driver\'s function names; ids and payload lengths are the immediates found at the writers\' call sites.',
           '// len: payload bytes after the 8-byte command header (-1: variable or not a single constant).', '']
    for kind, label in (('stream', 'kPvStreamOpcodes'), ('operation', 'kPvOperations')):
        out.append('static const PvOpcode %s[] = {' % label)
        for cid in sorted(tables[kind]):
            e = tables[kind][cid]
            ln = list(e['lens'])[0] if len(e['lens']) == 1 else -1
            out.append('\t{ 0x%03x, %d, "%s" },' % (cid, ln, '/'.join(e['names'])))
        out.append('};\n')
    out.append('static const PvFifoCommand kPvFifoCommands[] = {')
    for cid, name, fn in FIFO:
        out.append('\t{ 0x%02x, "%s" },   // %s' % (cid, name, fn))
    out.append('};')
    print('\n'.join(out))


if __name__ == '__main__':
    main()

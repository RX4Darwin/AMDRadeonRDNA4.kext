; Hand-written, AIR-shaped (same metadata and attribute conventions as Apple's compute kernels in the macOS 26.6.2 metallibs, see docs/m3-air-spike.md s.2);
; no Apple code. Equivalent MSL:
;   kernel void grid3d(device uint *out [[buffer(0)]], uint3 tid [[thread_position_in_grid]], uint3 gid [[threadgroup_position_in_grid]],
;                      uint3 lid [[thread_position_in_threadgroup]], uint3 grid [[threads_per_grid]]) {
;     uint idx = (tid.z * grid.y + tid.y) * grid.x + tid.x;  device uint *p = out + idx * 9;
;     p[0..2] = tid; p[3..5] = gid; p[6..8] = lid; }
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.6.0"

define void @grid3d(ptr addrspace(1) noundef "air-buffer-no-alias" %out, <3 x i32> noundef %tid, <3 x i32> noundef %gid, <3 x i32> noundef %lid, <3 x i32> noundef %grid) local_unnamed_addr #0 {
  %tx = extractelement <3 x i32> %tid, i32 0
  %ty = extractelement <3 x i32> %tid, i32 1
  %tz = extractelement <3 x i32> %tid, i32 2
  %gx = extractelement <3 x i32> %grid, i32 0
  %gy = extractelement <3 x i32> %grid, i32 1
  %a = mul i32 %tz, %gy
  %b = add i32 %a, %ty
  %c = mul i32 %b, %gx
  %idx = add i32 %c, %tx
  %o = mul i32 %idx, 9
  %o64 = zext i32 %o to i64
  %p = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %o64
  %v0 = extractelement <3 x i32> %tid, i32 0
  %v1 = extractelement <3 x i32> %tid, i32 1
  %v2 = extractelement <3 x i32> %tid, i32 2
  %v3 = extractelement <3 x i32> %gid, i32 0
  %v4 = extractelement <3 x i32> %gid, i32 1
  %v5 = extractelement <3 x i32> %gid, i32 2
  %v6 = extractelement <3 x i32> %lid, i32 0
  %v7 = extractelement <3 x i32> %lid, i32 1
  %v8 = extractelement <3 x i32> %lid, i32 2
  store i32 %v0, ptr addrspace(1) %p, align 4
  %p1 = getelementptr inbounds i32, ptr addrspace(1) %p, i64 1
  store i32 %v1, ptr addrspace(1) %p1, align 4
  %p2 = getelementptr inbounds i32, ptr addrspace(1) %p, i64 2
  store i32 %v2, ptr addrspace(1) %p2, align 4
  %p3 = getelementptr inbounds i32, ptr addrspace(1) %p, i64 3
  store i32 %v3, ptr addrspace(1) %p3, align 4
  %p4 = getelementptr inbounds i32, ptr addrspace(1) %p, i64 4
  store i32 %v4, ptr addrspace(1) %p4, align 4
  %p5 = getelementptr inbounds i32, ptr addrspace(1) %p, i64 5
  store i32 %v5, ptr addrspace(1) %p5, align 4
  %p6 = getelementptr inbounds i32, ptr addrspace(1) %p, i64 6
  store i32 %v6, ptr addrspace(1) %p6, align 4
  %p7 = getelementptr inbounds i32, ptr addrspace(1) %p, i64 7
  store i32 %v7, ptr addrspace(1) %p7, align 4
  %p8 = getelementptr inbounds i32, ptr addrspace(1) %p, i64 8
  store i32 %v8, ptr addrspace(1) %p8, align 4
  ret void
}

attributes #0 = { mustprogress nounwind "no-builtins" }

!llvm.module.flags = !{!0, !1}
!air.version = !{!2}
!air.language_version = !{!3}
!air.kernel = !{!4}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 7, !"air.max_device_buffers", i32 31}
!2 = !{i32 2, i32 8, i32 0}
!3 = !{!"Metal", i32 4, i32 0, i32 0}
!4 = !{ptr @grid3d, !5, !6}
!5 = !{}
!6 = !{!7, !8, !9, !10, !11}
!7 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!8 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint3", !"air.arg_name", !"tid"}
!9 = !{i32 2, !"air.threadgroup_position_in_grid", !"air.arg_type_name", !"uint3", !"air.arg_name", !"gid"}
!10 = !{i32 3, !"air.thread_position_in_threadgroup", !"air.arg_type_name", !"uint3", !"air.arg_name", !"lid"}
!11 = !{i32 4, !"air.threads_per_grid", !"air.arg_type_name", !"uint3", !"air.arg_name", !"grid"}

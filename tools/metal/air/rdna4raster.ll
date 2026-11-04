; ModuleID = 'rdna4raster.c'
source_filename = "rdna4raster.c"
target datalayout = "e-m:e-p:64:64-p1:64:64-p2:32:32-p3:32:32-p4:64:64-p5:32:32-p6:32:32-p7:160:256:256:32-p8:128:128:128:48-p9:192:256:256:32-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-v2048:2048-n32:64-S32-A5-G1-ni:7:8:9"
target triple = "amdgcn-amd-amdhsa"

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: write) uwtable
define hidden void @rdna4_draw_init(ptr nofree noundef writeonly captures(none) initializes((0, 84)) %0, ptr addrspace(1) noundef %1, i32 noundef %2, i32 noundef %3, i32 noundef %4, i32 noundef %5, ptr addrspace(1) noundef %6, i32 noundef %7, i32 noundef %8, i32 noundef %9, i32 noundef %10, float noundef %11, float noundef %12, float noundef %13, float noundef %14, i32 noundef %15, i32 noundef %16, i32 noundef %17, i32 noundef %18, i32 noundef %19) local_unnamed_addr #0 {
  store ptr addrspace(1) %1, ptr %0, align 8, !tbaa !9
  %21 = getelementptr inbounds nuw i8, ptr %0, i64 8
  store i32 %2, ptr %21, align 8, !tbaa !15
  %22 = getelementptr inbounds nuw i8, ptr %0, i64 12
  store i32 %3, ptr %22, align 4, !tbaa !16
  %23 = getelementptr inbounds nuw i8, ptr %0, i64 16
  store i32 %4, ptr %23, align 8, !tbaa !17
  %24 = getelementptr inbounds nuw i8, ptr %0, i64 20
  store i32 %5, ptr %24, align 4, !tbaa !18
  %25 = getelementptr inbounds nuw i8, ptr %0, i64 24
  store ptr addrspace(1) %6, ptr %25, align 8, !tbaa !19
  %26 = getelementptr inbounds nuw i8, ptr %0, i64 32
  store i32 %7, ptr %26, align 8, !tbaa !20
  %27 = getelementptr inbounds nuw i8, ptr %0, i64 36
  store i32 %8, ptr %27, align 4, !tbaa !21
  %28 = getelementptr inbounds nuw i8, ptr %0, i64 40
  store i32 %9, ptr %28, align 8, !tbaa !22
  %29 = getelementptr inbounds nuw i8, ptr %0, i64 44
  store i32 %10, ptr %29, align 4, !tbaa !23
  %30 = getelementptr inbounds nuw i8, ptr %0, i64 48
  store float %11, ptr %30, align 8, !tbaa !24
  %31 = getelementptr inbounds nuw i8, ptr %0, i64 52
  store float %12, ptr %31, align 4, !tbaa !25
  %32 = getelementptr inbounds nuw i8, ptr %0, i64 56
  store float %13, ptr %32, align 8, !tbaa !26
  %33 = getelementptr inbounds nuw i8, ptr %0, i64 60
  store float %14, ptr %33, align 4, !tbaa !27
  %34 = getelementptr inbounds nuw i8, ptr %0, i64 64
  store i32 %15, ptr %34, align 8, !tbaa !28
  %35 = getelementptr inbounds nuw i8, ptr %0, i64 68
  store i32 %16, ptr %35, align 4, !tbaa !29
  %36 = getelementptr inbounds nuw i8, ptr %0, i64 72
  store i32 %17, ptr %36, align 8, !tbaa !30
  %37 = getelementptr inbounds nuw i8, ptr %0, i64 76
  store i32 %18, ptr %37, align 4, !tbaa !31
  %38 = getelementptr inbounds nuw i8, ptr %0, i64 80
  store i32 %19, ptr %38, align 8, !tbaa !32
  ret void
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(read, inaccessiblemem: none, target_mem: none) uwtable
define hidden <4 x float> @rdna4_sample_2d(ptr addrspace(1) nofree noundef readonly captures(none) %0, ptr addrspace(4) nofree noundef readonly captures(none) %1, <2 x float> noundef %2) local_unnamed_addr #1 {
  %4 = load i32, ptr addrspace(4) %1, align 4, !tbaa !33
  %5 = lshr i32 %4, 5
  %6 = and i32 %5, 7
  %7 = lshr i32 %4, 8
  %8 = and i32 %7, 7
  %9 = icmp sgt i32 %4, -1
  %10 = extractelement <2 x float> %2, i64 0
  br i1 %9, label %16, label %11

11:                                               ; preds = %3
  %12 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 8
  %13 = load i32, ptr addrspace(1) %12, align 8, !tbaa !35
  %14 = uitofp contract i32 %13 to float
  %15 = fmul contract float %10, %14
  br label %16

16:                                               ; preds = %3, %11
  %17 = phi contract float [ %15, %11 ], [ %10, %3 ]
  %18 = extractelement <2 x float> %2, i64 1
  br i1 %9, label %24, label %19

19:                                               ; preds = %16
  %20 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 12
  %21 = load i32, ptr addrspace(1) %20, align 4, !tbaa !37
  %22 = uitofp contract i32 %21 to float
  %23 = fmul contract float %18, %22
  br label %24

24:                                               ; preds = %16, %19
  %25 = phi contract float [ %23, %19 ], [ %18, %16 ]
  %26 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 8
  %27 = load i32, ptr addrspace(1) %26, align 8, !tbaa !35
  %28 = icmp eq i32 %27, 0
  br i1 %28, label %462, label %29

29:                                               ; preds = %24
  %30 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 12
  %31 = load i32, ptr addrspace(1) %30, align 4, !tbaa !37
  %32 = icmp eq i32 %31, 0
  br i1 %32, label %462, label %33

33:                                               ; preds = %29
  %34 = and i32 %4, 12
  %35 = icmp eq i32 %34, 0
  br i1 %35, label %36, label %142

36:                                               ; preds = %33
  %37 = tail call contract float @llvm.floor.f32(float %17)
  %38 = fptosi float %37 to i32
  switch i32 %6, label %63 [
    i32 2, label %39
    i32 3, label %44
    i32 1, label %54
    i32 4, label %59
  ]

39:                                               ; preds = %36
  %40 = srem i32 %38, %27
  %41 = icmp slt i32 %40, 0
  %42 = select i1 %41, i32 %27, i32 0
  %43 = add nsw i32 %42, %40
  br label %68

44:                                               ; preds = %36
  %45 = shl nsw i32 %27, 1
  %46 = srem i32 %38, %45
  %47 = icmp slt i32 %46, 0
  %48 = select i1 %47, i32 %45, i32 0
  %49 = add nsw i32 %48, %46
  %50 = icmp slt i32 %49, %27
  %51 = xor i32 %49, -1
  %52 = add i32 %45, %51
  %53 = select i1 %50, i32 %49, i32 %52
  br label %68

54:                                               ; preds = %36
  %55 = ashr i32 %38, 31
  %56 = xor i32 %55, %38
  %57 = add nsw i32 %27, -1
  %58 = tail call i32 @llvm.smin.i32(i32 %56, i32 %57)
  br label %68

59:                                               ; preds = %36
  %60 = icmp sgt i32 %27, %38
  %61 = tail call i32 @llvm.smax.i32(i32 %38, i32 -1)
  %62 = select i1 %60, i32 %61, i32 -1
  br label %68

63:                                               ; preds = %36
  %64 = icmp slt i32 %38, 0
  %65 = add nsw i32 %27, -1
  %66 = tail call i32 @llvm.smin.i32(i32 %38, i32 %65)
  %67 = select i1 %64, i32 0, i32 %66
  br label %68

68:                                               ; preds = %39, %44, %54, %59, %63
  %69 = phi i32 [ %67, %63 ], [ %43, %39 ], [ %53, %44 ], [ %58, %54 ], [ %62, %59 ]
  %70 = tail call contract float @llvm.floor.f32(float %25)
  %71 = fptosi float %70 to i32
  switch i32 %8, label %96 [
    i32 2, label %72
    i32 3, label %77
    i32 1, label %87
    i32 4, label %92
  ]

72:                                               ; preds = %68
  %73 = srem i32 %71, %31
  %74 = icmp slt i32 %73, 0
  %75 = select i1 %74, i32 %31, i32 0
  %76 = add nsw i32 %75, %73
  br label %101

77:                                               ; preds = %68
  %78 = shl nsw i32 %31, 1
  %79 = srem i32 %71, %78
  %80 = icmp slt i32 %79, 0
  %81 = select i1 %80, i32 %78, i32 0
  %82 = add nsw i32 %81, %79
  %83 = icmp slt i32 %82, %31
  %84 = xor i32 %82, -1
  %85 = add i32 %78, %84
  %86 = select i1 %83, i32 %82, i32 %85
  br label %101

87:                                               ; preds = %68
  %88 = ashr i32 %71, 31
  %89 = xor i32 %88, %71
  %90 = add nsw i32 %31, -1
  %91 = tail call i32 @llvm.smin.i32(i32 %89, i32 %90)
  br label %101

92:                                               ; preds = %68
  %93 = icmp sgt i32 %31, %71
  %94 = tail call i32 @llvm.smax.i32(i32 %71, i32 -1)
  %95 = select i1 %93, i32 %94, i32 -1
  br label %101

96:                                               ; preds = %68
  %97 = icmp slt i32 %71, 0
  %98 = add nsw i32 %31, -1
  %99 = tail call i32 @llvm.smin.i32(i32 %71, i32 %98)
  %100 = select i1 %97, i32 0, i32 %99
  br label %101

101:                                              ; preds = %72, %77, %87, %92, %96
  %102 = phi i32 [ %100, %96 ], [ %76, %72 ], [ %86, %77 ], [ %91, %87 ], [ %95, %92 ]
  %103 = or i32 %102, %69
  %104 = icmp sgt i32 %103, -1
  br i1 %104, label %105, label %462

105:                                              ; preds = %101
  %106 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %107 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %108 = load i32, ptr addrspace(1) %107, align 8, !tbaa !39
  %109 = lshr i32 %108, 2
  %110 = mul i32 %109, %102
  %111 = add i32 %110, %69
  %112 = zext i32 %111 to i64
  %113 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %106, i64 %112
  %114 = load i32, ptr addrspace(1) %113, align 4, !tbaa !40
  %115 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %116 = load i32, ptr addrspace(1) %115, align 4, !tbaa !41
  %117 = and i32 %114, 255
  %118 = uitofp contract nneg i32 %117 to float
  %119 = fdiv contract float %118, 2.550000e+02
  %120 = lshr i32 %114, 8
  %121 = and i32 %120, 255
  %122 = uitofp contract nneg i32 %121 to float
  %123 = fdiv contract float %122, 2.550000e+02
  %124 = lshr i32 %114, 16
  %125 = and i32 %124, 255
  %126 = uitofp contract nneg i32 %125 to float
  %127 = fdiv contract float %126, 2.550000e+02
  %128 = lshr i32 %114, 24
  %129 = uitofp contract nneg i32 %128 to float
  %130 = fdiv contract float %129, 2.550000e+02
  %131 = and i32 %116, -2
  %132 = icmp eq i32 %131, 80
  %133 = insertelement <4 x float> poison, float %127, i64 0
  %134 = insertelement <4 x float> %133, float %123, i64 1
  %135 = insertelement <4 x float> %134, float %119, i64 2
  %136 = insertelement <4 x float> %135, float %130, i64 3
  %137 = insertelement <4 x float> poison, float %119, i64 0
  %138 = insertelement <4 x float> %137, float %123, i64 1
  %139 = insertelement <4 x float> %138, float %127, i64 2
  %140 = insertelement <4 x float> %139, float %130, i64 3
  %141 = select i1 %132, <4 x float> %136, <4 x float> %140
  br label %462

142:                                              ; preds = %33
  %143 = fadd contract float %17, -5.000000e-01
  %144 = fadd contract float %25, -5.000000e-01
  %145 = tail call contract float @llvm.floor.f32(float %143)
  %146 = fptosi float %145 to i32
  %147 = tail call contract float @llvm.floor.f32(float %144)
  %148 = fptosi float %147 to i32
  %149 = sitofp contract i32 %146 to float
  %150 = fsub contract float %143, %149
  %151 = sitofp contract i32 %148 to float
  %152 = fsub contract float %144, %151
  switch i32 %6, label %177 [
    i32 2, label %153
    i32 3, label %158
    i32 1, label %168
    i32 4, label %173
  ]

153:                                              ; preds = %142
  %154 = srem i32 %146, %27
  %155 = icmp slt i32 %154, 0
  %156 = select i1 %155, i32 %27, i32 0
  %157 = add nsw i32 %156, %154
  br label %182

158:                                              ; preds = %142
  %159 = shl nsw i32 %27, 1
  %160 = srem i32 %146, %159
  %161 = icmp slt i32 %160, 0
  %162 = select i1 %161, i32 %159, i32 0
  %163 = add nsw i32 %162, %160
  %164 = icmp slt i32 %163, %27
  %165 = xor i32 %163, -1
  %166 = add i32 %159, %165
  %167 = select i1 %164, i32 %163, i32 %166
  br label %182

168:                                              ; preds = %142
  %169 = ashr i32 %146, 31
  %170 = xor i32 %169, %146
  %171 = add nsw i32 %27, -1
  %172 = tail call i32 @llvm.smin.i32(i32 %170, i32 %171)
  br label %182

173:                                              ; preds = %142
  %174 = icmp sgt i32 %27, %146
  %175 = tail call i32 @llvm.smax.i32(i32 %146, i32 -1)
  %176 = select i1 %174, i32 %175, i32 -1
  br label %182

177:                                              ; preds = %142
  %178 = icmp slt i32 %146, 0
  %179 = add nsw i32 %27, -1
  %180 = tail call i32 @llvm.smin.i32(i32 %146, i32 %179)
  %181 = select i1 %178, i32 0, i32 %180
  br label %182

182:                                              ; preds = %153, %158, %168, %173, %177
  %183 = phi i32 [ %181, %177 ], [ %157, %153 ], [ %167, %158 ], [ %172, %168 ], [ %176, %173 ]
  %184 = add nsw i32 %146, 1
  switch i32 %6, label %209 [
    i32 2, label %185
    i32 3, label %190
    i32 1, label %200
    i32 4, label %205
  ]

185:                                              ; preds = %182
  %186 = srem i32 %184, %27
  %187 = icmp slt i32 %186, 0
  %188 = select i1 %187, i32 %27, i32 0
  %189 = add nsw i32 %188, %186
  br label %214

190:                                              ; preds = %182
  %191 = shl nsw i32 %27, 1
  %192 = srem i32 %184, %191
  %193 = icmp slt i32 %192, 0
  %194 = select i1 %193, i32 %191, i32 0
  %195 = add nsw i32 %194, %192
  %196 = icmp slt i32 %195, %27
  %197 = xor i32 %195, -1
  %198 = add i32 %191, %197
  %199 = select i1 %196, i32 %195, i32 %198
  br label %214

200:                                              ; preds = %182
  %201 = ashr i32 %184, 31
  %202 = xor i32 %201, %184
  %203 = add nsw i32 %27, -1
  %204 = tail call i32 @llvm.smin.i32(i32 %202, i32 %203)
  br label %214

205:                                              ; preds = %182
  %206 = icmp slt i32 %184, %27
  %207 = tail call i32 @llvm.smax.i32(i32 %184, i32 -1)
  %208 = select i1 %206, i32 %207, i32 -1
  br label %214

209:                                              ; preds = %182
  %210 = icmp slt i32 %146, -1
  %211 = add nsw i32 %27, -1
  %212 = tail call i32 @llvm.smin.i32(i32 %184, i32 %211)
  %213 = select i1 %210, i32 0, i32 %212
  br label %214

214:                                              ; preds = %185, %190, %200, %205, %209
  %215 = phi i32 [ %213, %209 ], [ %189, %185 ], [ %199, %190 ], [ %204, %200 ], [ %208, %205 ]
  switch i32 %8, label %240 [
    i32 2, label %216
    i32 3, label %221
    i32 1, label %231
    i32 4, label %236
  ]

216:                                              ; preds = %214
  %217 = srem i32 %148, %31
  %218 = icmp slt i32 %217, 0
  %219 = select i1 %218, i32 %31, i32 0
  %220 = add nsw i32 %219, %217
  br label %245

221:                                              ; preds = %214
  %222 = shl nsw i32 %31, 1
  %223 = srem i32 %148, %222
  %224 = icmp slt i32 %223, 0
  %225 = select i1 %224, i32 %222, i32 0
  %226 = add nsw i32 %225, %223
  %227 = icmp slt i32 %226, %31
  %228 = xor i32 %226, -1
  %229 = add i32 %222, %228
  %230 = select i1 %227, i32 %226, i32 %229
  br label %245

231:                                              ; preds = %214
  %232 = ashr i32 %148, 31
  %233 = xor i32 %232, %148
  %234 = add nsw i32 %31, -1
  %235 = tail call i32 @llvm.smin.i32(i32 %233, i32 %234)
  br label %245

236:                                              ; preds = %214
  %237 = icmp sgt i32 %31, %148
  %238 = tail call i32 @llvm.smax.i32(i32 %148, i32 -1)
  %239 = select i1 %237, i32 %238, i32 -1
  br label %245

240:                                              ; preds = %214
  %241 = icmp slt i32 %148, 0
  %242 = add nsw i32 %31, -1
  %243 = tail call i32 @llvm.smin.i32(i32 %148, i32 %242)
  %244 = select i1 %241, i32 0, i32 %243
  br label %245

245:                                              ; preds = %216, %221, %231, %236, %240
  %246 = phi i32 [ %244, %240 ], [ %220, %216 ], [ %230, %221 ], [ %235, %231 ], [ %239, %236 ]
  %247 = add nsw i32 %148, 1
  switch i32 %8, label %272 [
    i32 2, label %248
    i32 3, label %253
    i32 1, label %263
    i32 4, label %268
  ]

248:                                              ; preds = %245
  %249 = srem i32 %247, %31
  %250 = icmp slt i32 %249, 0
  %251 = select i1 %250, i32 %31, i32 0
  %252 = add nsw i32 %251, %249
  br label %277

253:                                              ; preds = %245
  %254 = shl nsw i32 %31, 1
  %255 = srem i32 %247, %254
  %256 = icmp slt i32 %255, 0
  %257 = select i1 %256, i32 %254, i32 0
  %258 = add nsw i32 %257, %255
  %259 = icmp slt i32 %258, %31
  %260 = xor i32 %258, -1
  %261 = add i32 %254, %260
  %262 = select i1 %259, i32 %258, i32 %261
  br label %277

263:                                              ; preds = %245
  %264 = ashr i32 %247, 31
  %265 = xor i32 %264, %247
  %266 = add nsw i32 %31, -1
  %267 = tail call i32 @llvm.smin.i32(i32 %265, i32 %266)
  br label %277

268:                                              ; preds = %245
  %269 = icmp slt i32 %247, %31
  %270 = tail call i32 @llvm.smax.i32(i32 %247, i32 -1)
  %271 = select i1 %269, i32 %270, i32 -1
  br label %277

272:                                              ; preds = %245
  %273 = icmp slt i32 %148, -1
  %274 = add nsw i32 %31, -1
  %275 = tail call i32 @llvm.smin.i32(i32 %247, i32 %274)
  %276 = select i1 %273, i32 0, i32 %275
  br label %277

277:                                              ; preds = %248, %253, %263, %268, %272
  %278 = phi i32 [ %276, %272 ], [ %252, %248 ], [ %262, %253 ], [ %267, %263 ], [ %271, %268 ]
  %279 = or i32 %246, %183
  %280 = icmp sgt i32 %279, -1
  br i1 %280, label %281, label %318

281:                                              ; preds = %277
  %282 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %283 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %284 = load i32, ptr addrspace(1) %283, align 8, !tbaa !39
  %285 = lshr i32 %284, 2
  %286 = mul i32 %285, %246
  %287 = add i32 %286, %183
  %288 = zext i32 %287 to i64
  %289 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %282, i64 %288
  %290 = load i32, ptr addrspace(1) %289, align 4, !tbaa !40
  %291 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %292 = load i32, ptr addrspace(1) %291, align 4, !tbaa !41
  %293 = and i32 %290, 255
  %294 = uitofp contract nneg i32 %293 to float
  %295 = fdiv contract float %294, 2.550000e+02
  %296 = lshr i32 %290, 8
  %297 = and i32 %296, 255
  %298 = uitofp contract nneg i32 %297 to float
  %299 = fdiv contract float %298, 2.550000e+02
  %300 = lshr i32 %290, 16
  %301 = and i32 %300, 255
  %302 = uitofp contract nneg i32 %301 to float
  %303 = fdiv contract float %302, 2.550000e+02
  %304 = lshr i32 %290, 24
  %305 = uitofp contract nneg i32 %304 to float
  %306 = fdiv contract float %305, 2.550000e+02
  %307 = and i32 %292, -2
  %308 = icmp eq i32 %307, 80
  %309 = insertelement <4 x float> poison, float %303, i64 0
  %310 = insertelement <4 x float> %309, float %299, i64 1
  %311 = insertelement <4 x float> %310, float %295, i64 2
  %312 = insertelement <4 x float> %311, float %306, i64 3
  %313 = insertelement <4 x float> poison, float %295, i64 0
  %314 = insertelement <4 x float> %313, float %299, i64 1
  %315 = insertelement <4 x float> %314, float %303, i64 2
  %316 = insertelement <4 x float> %315, float %306, i64 3
  %317 = select i1 %308, <4 x float> %312, <4 x float> %316
  br label %318

318:                                              ; preds = %277, %281
  %319 = phi <4 x float> [ %317, %281 ], [ zeroinitializer, %277 ]
  %320 = fsub contract float 1.000000e+00, %150
  %321 = insertelement <4 x float> poison, float %320, i64 0
  %322 = shufflevector <4 x float> %321, <4 x float> poison, <4 x i32> zeroinitializer
  %323 = fmul contract <4 x float> %322, %319
  %324 = or i32 %246, %215
  %325 = icmp sgt i32 %324, -1
  br i1 %325, label %326, label %363

326:                                              ; preds = %318
  %327 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %328 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %329 = load i32, ptr addrspace(1) %328, align 8, !tbaa !39
  %330 = lshr i32 %329, 2
  %331 = mul i32 %330, %246
  %332 = add i32 %331, %215
  %333 = zext i32 %332 to i64
  %334 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %327, i64 %333
  %335 = load i32, ptr addrspace(1) %334, align 4, !tbaa !40
  %336 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %337 = load i32, ptr addrspace(1) %336, align 4, !tbaa !41
  %338 = and i32 %335, 255
  %339 = uitofp contract nneg i32 %338 to float
  %340 = fdiv contract float %339, 2.550000e+02
  %341 = lshr i32 %335, 8
  %342 = and i32 %341, 255
  %343 = uitofp contract nneg i32 %342 to float
  %344 = fdiv contract float %343, 2.550000e+02
  %345 = lshr i32 %335, 16
  %346 = and i32 %345, 255
  %347 = uitofp contract nneg i32 %346 to float
  %348 = fdiv contract float %347, 2.550000e+02
  %349 = lshr i32 %335, 24
  %350 = uitofp contract nneg i32 %349 to float
  %351 = fdiv contract float %350, 2.550000e+02
  %352 = and i32 %337, -2
  %353 = icmp eq i32 %352, 80
  %354 = insertelement <4 x float> poison, float %348, i64 0
  %355 = insertelement <4 x float> %354, float %344, i64 1
  %356 = insertelement <4 x float> %355, float %340, i64 2
  %357 = insertelement <4 x float> %356, float %351, i64 3
  %358 = insertelement <4 x float> poison, float %340, i64 0
  %359 = insertelement <4 x float> %358, float %344, i64 1
  %360 = insertelement <4 x float> %359, float %348, i64 2
  %361 = insertelement <4 x float> %360, float %351, i64 3
  %362 = select i1 %353, <4 x float> %357, <4 x float> %361
  br label %363

363:                                              ; preds = %318, %326
  %364 = phi <4 x float> [ %362, %326 ], [ zeroinitializer, %318 ]
  %365 = insertelement <4 x float> poison, float %150, i64 0
  %366 = shufflevector <4 x float> %365, <4 x float> poison, <4 x i32> zeroinitializer
  %367 = fmul contract <4 x float> %366, %364
  %368 = fadd contract <4 x float> %323, %367
  %369 = or i32 %278, %183
  %370 = icmp sgt i32 %369, -1
  br i1 %370, label %371, label %408

371:                                              ; preds = %363
  %372 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %373 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %374 = load i32, ptr addrspace(1) %373, align 8, !tbaa !39
  %375 = lshr i32 %374, 2
  %376 = mul i32 %375, %278
  %377 = add i32 %376, %183
  %378 = zext i32 %377 to i64
  %379 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %372, i64 %378
  %380 = load i32, ptr addrspace(1) %379, align 4, !tbaa !40
  %381 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %382 = load i32, ptr addrspace(1) %381, align 4, !tbaa !41
  %383 = and i32 %380, 255
  %384 = uitofp contract nneg i32 %383 to float
  %385 = fdiv contract float %384, 2.550000e+02
  %386 = lshr i32 %380, 8
  %387 = and i32 %386, 255
  %388 = uitofp contract nneg i32 %387 to float
  %389 = fdiv contract float %388, 2.550000e+02
  %390 = lshr i32 %380, 16
  %391 = and i32 %390, 255
  %392 = uitofp contract nneg i32 %391 to float
  %393 = fdiv contract float %392, 2.550000e+02
  %394 = lshr i32 %380, 24
  %395 = uitofp contract nneg i32 %394 to float
  %396 = fdiv contract float %395, 2.550000e+02
  %397 = and i32 %382, -2
  %398 = icmp eq i32 %397, 80
  %399 = insertelement <4 x float> poison, float %393, i64 0
  %400 = insertelement <4 x float> %399, float %389, i64 1
  %401 = insertelement <4 x float> %400, float %385, i64 2
  %402 = insertelement <4 x float> %401, float %396, i64 3
  %403 = insertelement <4 x float> poison, float %385, i64 0
  %404 = insertelement <4 x float> %403, float %389, i64 1
  %405 = insertelement <4 x float> %404, float %393, i64 2
  %406 = insertelement <4 x float> %405, float %396, i64 3
  %407 = select i1 %398, <4 x float> %402, <4 x float> %406
  br label %408

408:                                              ; preds = %363, %371
  %409 = phi <4 x float> [ %407, %371 ], [ zeroinitializer, %363 ]
  %410 = fmul contract <4 x float> %322, %409
  %411 = or i32 %278, %215
  %412 = icmp sgt i32 %411, -1
  br i1 %412, label %413, label %450

413:                                              ; preds = %408
  %414 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %415 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %416 = load i32, ptr addrspace(1) %415, align 8, !tbaa !39
  %417 = lshr i32 %416, 2
  %418 = mul i32 %417, %278
  %419 = add i32 %418, %215
  %420 = zext i32 %419 to i64
  %421 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %414, i64 %420
  %422 = load i32, ptr addrspace(1) %421, align 4, !tbaa !40
  %423 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %424 = load i32, ptr addrspace(1) %423, align 4, !tbaa !41
  %425 = and i32 %422, 255
  %426 = uitofp contract nneg i32 %425 to float
  %427 = fdiv contract float %426, 2.550000e+02
  %428 = lshr i32 %422, 8
  %429 = and i32 %428, 255
  %430 = uitofp contract nneg i32 %429 to float
  %431 = fdiv contract float %430, 2.550000e+02
  %432 = lshr i32 %422, 16
  %433 = and i32 %432, 255
  %434 = uitofp contract nneg i32 %433 to float
  %435 = fdiv contract float %434, 2.550000e+02
  %436 = lshr i32 %422, 24
  %437 = uitofp contract nneg i32 %436 to float
  %438 = fdiv contract float %437, 2.550000e+02
  %439 = and i32 %424, -2
  %440 = icmp eq i32 %439, 80
  %441 = insertelement <4 x float> poison, float %435, i64 0
  %442 = insertelement <4 x float> %441, float %431, i64 1
  %443 = insertelement <4 x float> %442, float %427, i64 2
  %444 = insertelement <4 x float> %443, float %438, i64 3
  %445 = insertelement <4 x float> poison, float %427, i64 0
  %446 = insertelement <4 x float> %445, float %431, i64 1
  %447 = insertelement <4 x float> %446, float %435, i64 2
  %448 = insertelement <4 x float> %447, float %438, i64 3
  %449 = select i1 %440, <4 x float> %444, <4 x float> %448
  br label %450

450:                                              ; preds = %408, %413
  %451 = phi <4 x float> [ %449, %413 ], [ zeroinitializer, %408 ]
  %452 = fmul contract <4 x float> %366, %451
  %453 = fadd contract <4 x float> %410, %452
  %454 = fsub contract float 1.000000e+00, %152
  %455 = insertelement <4 x float> poison, float %454, i64 0
  %456 = shufflevector <4 x float> %455, <4 x float> poison, <4 x i32> zeroinitializer
  %457 = fmul contract <4 x float> %456, %368
  %458 = insertelement <4 x float> poison, float %152, i64 0
  %459 = shufflevector <4 x float> %458, <4 x float> poison, <4 x i32> zeroinitializer
  %460 = fmul contract <4 x float> %459, %453
  %461 = fadd contract <4 x float> %457, %460
  br label %462

462:                                              ; preds = %105, %101, %24, %29, %450
  %463 = phi <4 x float> [ zeroinitializer, %24 ], [ %461, %450 ], [ zeroinitializer, %29 ], [ %141, %105 ], [ zeroinitializer, %101 ]
  ret <4 x float> %463
}

; Function Attrs: mustprogress nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare float @llvm.floor.f32(float) #2

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(read, argmem: readwrite, inaccessiblemem: none, target_mem: none) uwtable
define hidden range(i32 0, 2) i32 @rdna4_px_begin(ptr nofree noundef readonly captures(none) %0, i32 noundef %1, i32 noundef %2, ptr nofree noundef writeonly captures(none) initializes((0, 8), (16, 36)) %3) local_unnamed_addr #3 {
  store i32 %1, ptr %3, align 16, !tbaa !42
  %5 = getelementptr inbounds nuw i8, ptr %3, i64 4
  store i32 %2, ptr %5, align 4, !tbaa !44
  %6 = getelementptr inbounds nuw i8, ptr %3, i64 16
  store <4 x float> zeroinitializer, ptr %6, align 16, !tbaa !45
  %7 = getelementptr inbounds nuw i8, ptr %3, i64 32
  store i32 0, ptr %7, align 16, !tbaa !46
  %8 = getelementptr inbounds nuw i8, ptr %0, i64 36
  %9 = load i32, ptr %8, align 4, !tbaa !21
  %10 = icmp ult i32 %1, %9
  br i1 %10, label %11, label %67

11:                                               ; preds = %4
  %12 = getelementptr inbounds nuw i8, ptr %0, i64 40
  %13 = load i32, ptr %12, align 8, !tbaa !22
  %14 = icmp ult i32 %2, %13
  br i1 %14, label %15, label %67

15:                                               ; preds = %11
  %16 = getelementptr inbounds nuw i8, ptr %0, i64 68
  %17 = load i32, ptr %16, align 4, !tbaa !29
  %18 = sub i32 %1, %17
  %19 = getelementptr inbounds nuw i8, ptr %0, i64 76
  %20 = load i32, ptr %19, align 4, !tbaa !31
  %21 = icmp ult i32 %18, %20
  br i1 %21, label %22, label %67

22:                                               ; preds = %15
  %23 = getelementptr inbounds nuw i8, ptr %0, i64 72
  %24 = load i32, ptr %23, align 8, !tbaa !30
  %25 = sub i32 %2, %24
  %26 = getelementptr inbounds nuw i8, ptr %0, i64 80
  %27 = load i32, ptr %26, align 8, !tbaa !32
  %28 = icmp ult i32 %25, %27
  br i1 %28, label %29, label %67

29:                                               ; preds = %22
  %30 = getelementptr inbounds nuw i8, ptr %0, i64 24
  %31 = load ptr addrspace(1), ptr %30, align 8, !tbaa !19
  %32 = getelementptr inbounds nuw i8, ptr %0, i64 32
  %33 = load i32, ptr %32, align 8, !tbaa !20
  %34 = lshr i32 %33, 2
  %35 = mul i32 %34, %2
  %36 = add i32 %35, %1
  %37 = zext i32 %36 to i64
  %38 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %31, i64 %37
  %39 = load i32, ptr addrspace(1) %38, align 4, !tbaa !40
  %40 = getelementptr inbounds nuw i8, ptr %0, i64 44
  %41 = load i32, ptr %40, align 4, !tbaa !23
  %42 = and i32 %39, 255
  %43 = uitofp contract nneg i32 %42 to float
  %44 = fdiv contract float %43, 2.550000e+02
  %45 = lshr i32 %39, 8
  %46 = and i32 %45, 255
  %47 = uitofp contract nneg i32 %46 to float
  %48 = fdiv contract float %47, 2.550000e+02
  %49 = lshr i32 %39, 16
  %50 = and i32 %49, 255
  %51 = uitofp contract nneg i32 %50 to float
  %52 = fdiv contract float %51, 2.550000e+02
  %53 = lshr i32 %39, 24
  %54 = uitofp contract nneg i32 %53 to float
  %55 = fdiv contract float %54, 2.550000e+02
  %56 = and i32 %41, -2
  %57 = icmp eq i32 %56, 80
  %58 = insertelement <4 x float> poison, float %52, i64 0
  %59 = insertelement <4 x float> %58, float %48, i64 1
  %60 = insertelement <4 x float> %59, float %44, i64 2
  %61 = insertelement <4 x float> %60, float %55, i64 3
  %62 = insertelement <4 x float> poison, float %44, i64 0
  %63 = insertelement <4 x float> %62, float %48, i64 1
  %64 = insertelement <4 x float> %63, float %52, i64 2
  %65 = insertelement <4 x float> %64, float %55, i64 3
  %66 = select i1 %57, <4 x float> %61, <4 x float> %65
  store <4 x float> %66, ptr %6, align 16, !tbaa !45
  br label %67

67:                                               ; preds = %4, %11, %15, %22, %29
  %68 = phi i32 [ 1, %29 ], [ 0, %22 ], [ 0, %15 ], [ 0, %11 ], [ 0, %4 ]
  ret i32 %68
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: read) uwtable
define hidden range(i32 0, -2) i32 @rdna4_tri_count(ptr nofree noundef readonly captures(none) %0) local_unnamed_addr #4 {
  %2 = getelementptr inbounds nuw i8, ptr %0, i64 16
  %3 = load i32, ptr %2, align 8, !tbaa !17
  switch i32 %3, label %12 [
    i32 4, label %4
    i32 3, label %8
  ]

4:                                                ; preds = %1
  %5 = getelementptr inbounds nuw i8, ptr %0, i64 20
  %6 = load i32, ptr %5, align 4, !tbaa !18
  %7 = tail call i32 @llvm.usub.sat.i32(i32 %6, i32 2)
  br label %12

8:                                                ; preds = %1
  %9 = getelementptr inbounds nuw i8, ptr %0, i64 20
  %10 = load i32, ptr %9, align 4, !tbaa !18
  %11 = udiv i32 %10, 3
  br label %12

12:                                               ; preds = %8, %1, %4
  %13 = phi i32 [ %7, %4 ], [ %11, %8 ], [ 0, %1 ]
  ret i32 %13
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(read, argmem: readwrite, inaccessiblemem: none, target_mem: none) uwtable
define hidden range(i32 0, 3) i32 @rdna4_tri_cover(ptr nofree noundef readonly captures(none) %0, ptr nofree noundef readonly captures(none) %1, i32 noundef %2, ptr nofree noundef writeonly captures(none) initializes((0, 24)) %3) local_unnamed_addr #3 {
  %5 = mul i32 %2, 3
  %6 = add i32 %5, 1
  %7 = load i32, ptr %1, align 16, !tbaa !42
  %8 = uitofp contract i32 %7 to float
  %9 = fadd contract float %8, 5.000000e-01
  %10 = getelementptr inbounds nuw i8, ptr %1, i64 4
  %11 = load i32, ptr %10, align 4, !tbaa !44
  %12 = uitofp contract i32 %11 to float
  %13 = fadd contract float %12, 5.000000e-01
  %14 = getelementptr inbounds nuw i8, ptr %0, i64 16
  %15 = load i32, ptr %14, align 8, !tbaa !17
  %16 = icmp eq i32 %15, 4
  %17 = and i32 %2, 1
  %18 = add i32 %17, %2
  %19 = or i32 %2, 1
  %20 = select i1 %16, i32 %2, i32 %5
  %21 = select i1 %16, i32 %19, i32 %6
  %22 = select i1 %16, i32 %18, i32 %5
  %23 = add i32 %20, 2
  %24 = load ptr addrspace(1), ptr %0, align 8, !tbaa !9
  %25 = getelementptr inbounds nuw i8, ptr %0, i64 8
  %26 = load i32, ptr %25, align 8, !tbaa !15
  %27 = mul i32 %22, %26
  %28 = zext i32 %27 to i64
  %29 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %24, i64 %28
  store ptr addrspace(1) %29, ptr %3, align 8, !tbaa !47
  %30 = load ptr addrspace(1), ptr %0, align 8, !tbaa !9
  %31 = mul i32 %21, %26
  %32 = zext i32 %31 to i64
  %33 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %30, i64 %32
  %34 = getelementptr inbounds nuw i8, ptr %3, i64 8
  store ptr addrspace(1) %33, ptr %34, align 8, !tbaa !47
  %35 = load ptr addrspace(1), ptr %0, align 8, !tbaa !9
  %36 = mul i32 %23, %26
  %37 = zext i32 %36 to i64
  %38 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %35, i64 %37
  %39 = getelementptr inbounds nuw i8, ptr %3, i64 16
  store ptr addrspace(1) %38, ptr %39, align 8, !tbaa !47
  %40 = getelementptr inbounds nuw i8, ptr %0, i64 12
  %41 = load i32, ptr %40, align 4, !tbaa !16
  %42 = add i32 %41, 3
  %43 = zext i32 %42 to i64
  %44 = zext i32 %41 to i64
  %45 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %29, i64 %43
  %46 = load float, ptr addrspace(1) %45, align 4, !tbaa !48
  %47 = fcmp contract ugt float %46, 0.000000e+00
  br i1 %47, label %48, label %186

48:                                               ; preds = %4
  %49 = getelementptr inbounds nuw i8, ptr %0, i64 60
  %50 = getelementptr inbounds nuw i8, ptr %0, i64 52
  %51 = getelementptr inbounds nuw i8, ptr %0, i64 56
  %52 = getelementptr inbounds nuw i8, ptr %0, i64 48
  %53 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %29, i64 %44
  %54 = getelementptr inbounds nuw i8, ptr addrspace(1) %53, i64 12
  %55 = load float, ptr addrspace(1) %54, align 4, !tbaa !48
  %56 = fdiv contract float 1.000000e+00, %55
  %57 = load float, ptr %52, align 8, !tbaa !24
  %58 = load float, ptr addrspace(1) %53, align 4, !tbaa !48
  %59 = fmul contract float %56, %58
  %60 = fmul contract float %59, 5.000000e-01
  %61 = fadd contract float %60, 5.000000e-01
  %62 = load float, ptr %51, align 8, !tbaa !26
  %63 = fmul contract float %62, %61
  %64 = fadd contract float %57, %63
  %65 = load float, ptr %50, align 4, !tbaa !25
  %66 = getelementptr inbounds nuw i8, ptr addrspace(1) %53, i64 4
  %67 = load float, ptr addrspace(1) %66, align 4, !tbaa !48
  %68 = fmul contract float %56, %67
  %69 = fmul contract float %68, 5.000000e-01
  %70 = fsub contract float 5.000000e-01, %69
  %71 = load float, ptr %49, align 4, !tbaa !27
  %72 = fmul contract float %71, %70
  %73 = fadd contract float %65, %72
  %74 = getelementptr inbounds nuw i8, ptr addrspace(1) %53, i64 8
  %75 = load float, ptr addrspace(1) %74, align 4, !tbaa !48
  %76 = fmul contract float %56, %75
  %77 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %33, i64 %43
  %78 = load float, ptr addrspace(1) %77, align 4, !tbaa !48
  %79 = fcmp contract ugt float %78, 0.000000e+00
  br i1 %79, label %80, label %186

80:                                               ; preds = %48
  %81 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %33, i64 %44
  %82 = getelementptr inbounds nuw i8, ptr addrspace(1) %81, i64 12
  %83 = load float, ptr addrspace(1) %82, align 4, !tbaa !48
  %84 = fdiv contract float 1.000000e+00, %83
  %85 = load float, ptr addrspace(1) %81, align 4, !tbaa !48
  %86 = fmul contract float %84, %85
  %87 = fmul contract float %86, 5.000000e-01
  %88 = fadd contract float %87, 5.000000e-01
  %89 = fmul contract float %62, %88
  %90 = fadd contract float %57, %89
  %91 = getelementptr inbounds nuw i8, ptr addrspace(1) %81, i64 4
  %92 = load float, ptr addrspace(1) %91, align 4, !tbaa !48
  %93 = fmul contract float %84, %92
  %94 = fmul contract float %93, 5.000000e-01
  %95 = fsub contract float 5.000000e-01, %94
  %96 = fmul contract float %71, %95
  %97 = fadd contract float %65, %96
  %98 = getelementptr inbounds nuw i8, ptr addrspace(1) %81, i64 8
  %99 = load float, ptr addrspace(1) %98, align 4, !tbaa !48
  %100 = fmul contract float %84, %99
  %101 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %38, i64 %43
  %102 = load float, ptr addrspace(1) %101, align 4, !tbaa !48
  %103 = fcmp contract ugt float %102, 0.000000e+00
  br i1 %103, label %104, label %186

104:                                              ; preds = %80
  %105 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %38, i64 %44
  %106 = getelementptr inbounds nuw i8, ptr addrspace(1) %105, i64 12
  %107 = load float, ptr addrspace(1) %106, align 4, !tbaa !48
  %108 = fdiv contract float 1.000000e+00, %107
  %109 = load float, ptr addrspace(1) %105, align 4, !tbaa !48
  %110 = fmul contract float %108, %109
  %111 = fmul contract float %110, 5.000000e-01
  %112 = fadd contract float %111, 5.000000e-01
  %113 = fmul contract float %62, %112
  %114 = fadd contract float %57, %113
  %115 = getelementptr inbounds nuw i8, ptr addrspace(1) %105, i64 4
  %116 = load float, ptr addrspace(1) %115, align 4, !tbaa !48
  %117 = fmul contract float %108, %116
  %118 = fmul contract float %117, 5.000000e-01
  %119 = fsub contract float 5.000000e-01, %118
  %120 = fmul contract float %71, %119
  %121 = fadd contract float %65, %120
  %122 = fsub contract float %90, %64
  %123 = fsub contract float %121, %73
  %124 = fmul contract float %123, %122
  %125 = fsub contract float %114, %64
  %126 = fsub contract float %97, %73
  %127 = fmul contract float %126, %125
  %128 = fsub contract float %124, %127
  %129 = fcmp contract oeq float %128, 0.000000e+00
  br i1 %129, label %186, label %130

130:                                              ; preds = %104
  %131 = getelementptr inbounds nuw i8, ptr addrspace(1) %105, i64 8
  %132 = load float, ptr addrspace(1) %131, align 4, !tbaa !48
  %133 = fmul contract float %108, %132
  %134 = fsub contract float %114, %90
  %135 = fsub contract float %13, %97
  %136 = fmul contract float %135, %134
  %137 = fsub contract float %121, %97
  %138 = fsub contract float %9, %90
  %139 = fmul contract float %137, %138
  %140 = fsub contract float %136, %139
  %141 = fsub contract float %64, %114
  %142 = fsub contract float %13, %121
  %143 = fmul contract float %142, %141
  %144 = fsub contract float %73, %121
  %145 = fsub contract float %9, %114
  %146 = fmul contract float %144, %145
  %147 = fsub contract float %143, %146
  %148 = fsub contract float %13, %73
  %149 = fmul contract float %148, %122
  %150 = fsub contract float %9, %64
  %151 = fmul contract float %126, %150
  %152 = fsub contract float %149, %151
  %153 = fdiv contract float %140, %128
  %154 = fdiv contract float %147, %128
  %155 = fdiv contract float %152, %128
  %156 = fmul contract float %56, %153
  %157 = fmul contract float %84, %154
  %158 = fadd contract float %156, %157
  %159 = fmul contract float %108, %155
  %160 = fadd contract float %159, %158
  %161 = getelementptr inbounds nuw i8, ptr %3, i64 36
  %162 = getelementptr inbounds nuw i8, ptr %3, i64 24
  store float %153, ptr %161, align 4, !tbaa !48
  %163 = fdiv contract float %156, %160
  store float %163, ptr %162, align 8, !tbaa !48
  %164 = getelementptr inbounds nuw i8, ptr %3, i64 40
  store float %154, ptr %164, align 8, !tbaa !48
  %165 = fdiv contract float %157, %160
  %166 = getelementptr inbounds nuw i8, ptr %3, i64 28
  store float %165, ptr %166, align 4, !tbaa !48
  %167 = getelementptr inbounds nuw i8, ptr %3, i64 44
  store float %155, ptr %167, align 4, !tbaa !48
  %168 = fdiv contract float %159, %160
  %169 = getelementptr inbounds nuw i8, ptr %3, i64 32
  store float %168, ptr %169, align 8, !tbaa !48
  %170 = fcmp contract olt float %153, 0.000000e+00
  %171 = fcmp contract olt float %154, 0.000000e+00
  %172 = fcmp contract olt float %155, 0.000000e+00
  %173 = select i1 %172, i1 true, i1 %171
  %174 = select i1 %173, i1 true, i1 %170
  %175 = fmul contract float %76, %153
  %176 = fmul contract float %100, %154
  %177 = fadd contract float %175, %176
  %178 = fmul contract float %133, %155
  %179 = fadd contract float %178, %177
  %180 = getelementptr inbounds nuw i8, ptr %3, i64 48
  store float %179, ptr %180, align 8, !tbaa !49
  %181 = getelementptr inbounds nuw i8, ptr %3, i64 52
  store float %160, ptr %181, align 4, !tbaa !51
  %182 = fcmp contract olt float %128, 0.000000e+00
  %183 = zext i1 %182 to i32
  %184 = getelementptr inbounds nuw i8, ptr %3, i64 56
  store i32 %183, ptr %184, align 8, !tbaa !52
  %185 = select i1 %174, i32 2, i32 1
  br label %186

186:                                              ; preds = %4, %48, %80, %104, %130
  %187 = phi i32 [ 0, %104 ], [ %185, %130 ], [ 0, %80 ], [ 0, %48 ], [ 0, %4 ]
  ret i32 %187
}

; Function Attrs: alwaysinline nofree norecurse nosync nounwind memory(read, inaccessiblemem: none, target_mem: none) uwtable
define hidden <4 x float> @rdna4_interp(ptr nofree noundef readonly captures(none) %0, i32 noundef %1, i32 noundef %2, i32 noundef %3) local_unnamed_addr #5 {
  %5 = tail call i32 @llvm.umin.i32(i32 %2, i32 4)
  %6 = icmp eq i32 %2, 0
  br i1 %6, label %19, label %7

7:                                                ; preds = %4
  %8 = load ptr addrspace(1), ptr %0, align 8, !tbaa !47
  %9 = getelementptr inbounds nuw i8, ptr %0, i64 8
  %10 = load ptr addrspace(1), ptr %9, align 8, !tbaa !47
  %11 = getelementptr inbounds nuw i8, ptr %0, i64 16
  %12 = load ptr addrspace(1), ptr %11, align 8, !tbaa !47
  %13 = getelementptr inbounds nuw i8, ptr %0, i64 36
  %14 = getelementptr inbounds nuw i8, ptr %0, i64 40
  %15 = getelementptr inbounds nuw i8, ptr %0, i64 44
  %16 = getelementptr inbounds nuw i8, ptr %0, i64 24
  %17 = getelementptr inbounds nuw i8, ptr %0, i64 28
  %18 = getelementptr inbounds nuw i8, ptr %0, i64 32
  br label %21

19:                                               ; preds = %50, %4
  %20 = phi <4 x float> [ zeroinitializer, %4 ], [ %52, %50 ]
  ret <4 x float> %20

21:                                               ; preds = %7, %50
  %22 = phi <4 x float> [ zeroinitializer, %7 ], [ %52, %50 ]
  %23 = phi i32 [ 0, %7 ], [ %53, %50 ]
  %24 = add i32 %23, %1
  %25 = zext i32 %24 to i64
  %26 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %8, i64 %25
  %27 = load float, ptr addrspace(1) %26, align 4, !tbaa !48
  %28 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %10, i64 %25
  %29 = load float, ptr addrspace(1) %28, align 4, !tbaa !48
  %30 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %12, i64 %25
  %31 = load float, ptr addrspace(1) %30, align 4, !tbaa !48
  switch i32 %3, label %41 [
    i32 2, label %50
    i32 1, label %32
  ]

32:                                               ; preds = %21
  %33 = load float, ptr %13, align 4, !tbaa !48
  %34 = fmul contract float %27, %33
  %35 = load float, ptr %14, align 8, !tbaa !48
  %36 = fmul contract float %29, %35
  %37 = fadd contract float %34, %36
  %38 = load float, ptr %15, align 4, !tbaa !48
  %39 = fmul contract float %31, %38
  %40 = fadd contract float %37, %39
  br label %50

41:                                               ; preds = %21
  %42 = load float, ptr %16, align 8, !tbaa !48
  %43 = fmul contract float %27, %42
  %44 = load float, ptr %17, align 4, !tbaa !48
  %45 = fmul contract float %29, %44
  %46 = fadd contract float %43, %45
  %47 = load float, ptr %18, align 8, !tbaa !48
  %48 = fmul contract float %31, %47
  %49 = fadd contract float %46, %48
  br label %50

50:                                               ; preds = %21, %32, %41
  %51 = phi float [ %49, %41 ], [ %40, %32 ], [ %27, %21 ]
  %52 = insertelement <4 x float> %22, float %51, i32 %23
  %53 = add nuw nsw i32 %23, 1
  %54 = icmp eq i32 %53, %5
  br i1 %54, label %19, label %21, !llvm.loop !53
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: read) uwtable
define hidden <4 x float> @rdna4_frag_position(ptr nofree noundef readonly captures(none) %0, ptr nofree noundef readonly captures(none) %1) local_unnamed_addr #4 {
  %3 = load i32, ptr %0, align 16, !tbaa !42
  %4 = uitofp contract i32 %3 to float
  %5 = fadd contract float %4, 5.000000e-01
  %6 = insertelement <4 x float> poison, float %5, i64 0
  %7 = getelementptr inbounds nuw i8, ptr %0, i64 4
  %8 = load i32, ptr %7, align 4, !tbaa !44
  %9 = uitofp contract i32 %8 to float
  %10 = fadd contract float %9, 5.000000e-01
  %11 = insertelement <4 x float> %6, float %10, i64 1
  %12 = getelementptr inbounds nuw i8, ptr %1, i64 48
  %13 = load float, ptr %12, align 8, !tbaa !49
  %14 = insertelement <4 x float> %11, float %13, i64 2
  %15 = getelementptr inbounds nuw i8, ptr %1, i64 52
  %16 = load float, ptr %15, align 4, !tbaa !51
  %17 = insertelement <4 x float> %14, float %16, i64 3
  ret <4 x float> %17
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: read) uwtable
define hidden i32 @rdna4_front_facing(ptr nofree noundef readonly captures(none) %0) local_unnamed_addr #4 {
  %2 = getelementptr inbounds nuw i8, ptr %0, i64 56
  %3 = load i32, ptr %2, align 8, !tbaa !52
  ret i32 %3
}

; Function Attrs: alwaysinline nofree norecurse nosync nounwind memory(argmem: readwrite) uwtable
define hidden void @rdna4_px_blend(ptr nofree noundef readonly captures(none) %0, ptr nofree noundef captures(none) initializes((32, 36)) %1, <4 x float> noundef %2) local_unnamed_addr #6 {
  %4 = getelementptr inbounds nuw i8, ptr %1, i64 32
  store i32 1, ptr %4, align 16, !tbaa !46
  %5 = getelementptr inbounds nuw i8, ptr %0, i64 64
  %6 = load i32, ptr %5, align 8, !tbaa !28
  %7 = icmp sgt i32 %6, -1
  %8 = getelementptr inbounds nuw i8, ptr %1, i64 16
  br i1 %7, label %168, label %9

9:                                                ; preds = %3
  %10 = load <4 x float>, ptr %8, align 16, !tbaa !45
  %11 = extractelement <4 x float> %2, i64 3
  %12 = extractelement <4 x float> %10, i64 3
  %13 = fsub contract float 1.000000e+00, %12
  %14 = fcmp contract olt float %11, %13
  %15 = select contract i1 %14, float %11, float %13
  %16 = fsub contract float 1.000000e+00, %11
  %17 = lshr i32 %6, 8
  %18 = trunc i32 %6 to i8
  %19 = trunc i32 %17 to i8
  %20 = and i8 %19, 127
  %21 = extractelement <4 x float> %2, i64 0
  switch i8 %18, label %70 [
    i8 0, label %71
    i8 2, label %57
    i8 3, label %58
    i8 4, label %60
    i8 5, label %61
    i8 6, label %62
    i8 7, label %64
    i8 8, label %67
    i8 9, label %68
    i8 10, label %69
  ]

22:                                               ; preds = %159
  br label %35

23:                                               ; preds = %159
  br label %35

24:                                               ; preds = %159
  %25 = extractelement <4 x float> %10, i64 3
  %26 = fsub contract float 1.000000e+00, %25
  br label %35

27:                                               ; preds = %159
  %28 = extractelement <4 x float> %10, i64 3
  br label %35

29:                                               ; preds = %159
  br label %35

30:                                               ; preds = %159
  br label %35

31:                                               ; preds = %159
  %32 = fsub contract float 1.000000e+00, %166
  br label %35

33:                                               ; preds = %159
  br label %35

34:                                               ; preds = %159
  br label %35

35:                                               ; preds = %34, %33, %31, %30, %29, %27, %24, %23, %22, %159
  %36 = phi float [ 1.000000e+00, %34 ], [ 0.000000e+00, %159 ], [ %166, %33 ], [ %32, %31 ], [ %11, %30 ], [ %16, %29 ], [ %28, %27 ], [ %26, %24 ], [ %12, %23 ], [ %13, %22 ]
  %37 = fmul contract float %166, %36
  %38 = extractelement <4 x float> %10, i64 3
  %39 = trunc nuw i32 %165 to i8
  %40 = and i8 %39, 127
  switch i8 %40, label %51 [
    i8 0, label %52
    i8 2, label %50
    i8 3, label %48
    i8 4, label %47
    i8 5, label %46
    i8 6, label %45
    i8 7, label %43
    i8 8, label %42
    i8 9, label %41
  ]

41:                                               ; preds = %35
  br label %52

42:                                               ; preds = %35
  br label %52

43:                                               ; preds = %35
  %44 = fsub contract float 1.000000e+00, %38
  br label %52

45:                                               ; preds = %35
  br label %52

46:                                               ; preds = %35
  br label %52

47:                                               ; preds = %35
  br label %52

48:                                               ; preds = %35
  %49 = fsub contract float 1.000000e+00, %166
  br label %52

50:                                               ; preds = %35
  br label %52

51:                                               ; preds = %35
  br label %52

52:                                               ; preds = %51, %50, %48, %47, %46, %45, %43, %42, %41, %35
  %53 = phi float [ 1.000000e+00, %51 ], [ 0.000000e+00, %35 ], [ %166, %50 ], [ %49, %48 ], [ %11, %47 ], [ %16, %46 ], [ %38, %45 ], [ %44, %43 ], [ %12, %42 ], [ %13, %41 ]
  %54 = fmul contract float %38, %53
  %55 = fadd contract float %37, %54
  %56 = insertelement <4 x float> %163, float %55, i64 3
  br label %168

57:                                               ; preds = %9
  br label %71

58:                                               ; preds = %9
  %59 = fsub contract float 1.000000e+00, %21
  br label %71

60:                                               ; preds = %9
  br label %71

61:                                               ; preds = %9
  br label %71

62:                                               ; preds = %9
  %63 = extractelement <4 x float> %10, i64 0
  br label %71

64:                                               ; preds = %9
  %65 = extractelement <4 x float> %10, i64 0
  %66 = fsub contract float 1.000000e+00, %65
  br label %71

67:                                               ; preds = %9
  br label %71

68:                                               ; preds = %9
  br label %71

69:                                               ; preds = %9
  br label %71

70:                                               ; preds = %9
  br label %71

71:                                               ; preds = %9, %57, %58, %60, %61, %62, %64, %67, %68, %69, %70
  %72 = phi float [ 1.000000e+00, %70 ], [ %15, %69 ], [ %21, %57 ], [ %59, %58 ], [ %11, %60 ], [ %16, %61 ], [ %63, %62 ], [ %66, %64 ], [ %12, %67 ], [ %13, %68 ], [ 0.000000e+00, %9 ]
  %73 = fmul contract float %21, %72
  %74 = extractelement <4 x float> %10, i64 0
  switch i8 %20, label %86 [
    i8 0, label %87
    i8 2, label %75
    i8 3, label %76
    i8 4, label %78
    i8 5, label %79
    i8 6, label %80
    i8 7, label %81
    i8 8, label %83
    i8 9, label %84
    i8 10, label %85
  ]

75:                                               ; preds = %71
  br label %87

76:                                               ; preds = %71
  %77 = fsub contract float 1.000000e+00, %21
  br label %87

78:                                               ; preds = %71
  br label %87

79:                                               ; preds = %71
  br label %87

80:                                               ; preds = %71
  br label %87

81:                                               ; preds = %71
  %82 = fsub contract float 1.000000e+00, %74
  br label %87

83:                                               ; preds = %71
  br label %87

84:                                               ; preds = %71
  br label %87

85:                                               ; preds = %71
  br label %87

86:                                               ; preds = %71
  br label %87

87:                                               ; preds = %71, %75, %76, %78, %79, %80, %81, %83, %84, %85, %86
  %88 = phi float [ 1.000000e+00, %86 ], [ %15, %85 ], [ %21, %75 ], [ %77, %76 ], [ %11, %78 ], [ %16, %79 ], [ %74, %80 ], [ %82, %81 ], [ %12, %83 ], [ %13, %84 ], [ 0.000000e+00, %71 ]
  %89 = fmul contract float %74, %88
  %90 = fadd contract float %73, %89
  %91 = insertelement <4 x float> poison, float %90, i64 0
  %92 = extractelement <4 x float> %2, i64 1
  switch i8 %18, label %106 [
    i8 0, label %107
    i8 2, label %105
    i8 3, label %103
    i8 4, label %102
    i8 5, label %101
    i8 6, label %99
    i8 7, label %96
    i8 8, label %95
    i8 9, label %94
    i8 10, label %93
  ]

93:                                               ; preds = %87
  br label %107

94:                                               ; preds = %87
  br label %107

95:                                               ; preds = %87
  br label %107

96:                                               ; preds = %87
  %97 = extractelement <4 x float> %10, i64 1
  %98 = fsub contract float 1.000000e+00, %97
  br label %107

99:                                               ; preds = %87
  %100 = extractelement <4 x float> %10, i64 1
  br label %107

101:                                              ; preds = %87
  br label %107

102:                                              ; preds = %87
  br label %107

103:                                              ; preds = %87
  %104 = fsub contract float 1.000000e+00, %92
  br label %107

105:                                              ; preds = %87
  br label %107

106:                                              ; preds = %87
  br label %107

107:                                              ; preds = %106, %105, %103, %102, %101, %99, %96, %95, %94, %93, %87
  %108 = phi float [ 1.000000e+00, %106 ], [ %15, %93 ], [ %92, %105 ], [ %104, %103 ], [ %11, %102 ], [ %16, %101 ], [ %100, %99 ], [ %98, %96 ], [ %12, %95 ], [ %13, %94 ], [ 0.000000e+00, %87 ]
  %109 = fmul contract float %92, %108
  %110 = extractelement <4 x float> %10, i64 1
  switch i8 %20, label %122 [
    i8 0, label %123
    i8 2, label %121
    i8 3, label %119
    i8 4, label %118
    i8 5, label %117
    i8 6, label %116
    i8 7, label %114
    i8 8, label %113
    i8 9, label %112
    i8 10, label %111
  ]

111:                                              ; preds = %107
  br label %123

112:                                              ; preds = %107
  br label %123

113:                                              ; preds = %107
  br label %123

114:                                              ; preds = %107
  %115 = fsub contract float 1.000000e+00, %110
  br label %123

116:                                              ; preds = %107
  br label %123

117:                                              ; preds = %107
  br label %123

118:                                              ; preds = %107
  br label %123

119:                                              ; preds = %107
  %120 = fsub contract float 1.000000e+00, %92
  br label %123

121:                                              ; preds = %107
  br label %123

122:                                              ; preds = %107
  br label %123

123:                                              ; preds = %122, %121, %119, %118, %117, %116, %114, %113, %112, %111, %107
  %124 = phi float [ 1.000000e+00, %122 ], [ %15, %111 ], [ %92, %121 ], [ %120, %119 ], [ %11, %118 ], [ %16, %117 ], [ %110, %116 ], [ %115, %114 ], [ %12, %113 ], [ %13, %112 ], [ 0.000000e+00, %107 ]
  %125 = fmul contract float %110, %124
  %126 = fadd contract float %109, %125
  %127 = insertelement <4 x float> %91, float %126, i64 1
  %128 = extractelement <4 x float> %2, i64 2
  switch i8 %18, label %142 [
    i8 0, label %143
    i8 2, label %141
    i8 3, label %139
    i8 4, label %138
    i8 5, label %137
    i8 6, label %135
    i8 7, label %132
    i8 8, label %131
    i8 9, label %130
    i8 10, label %129
  ]

129:                                              ; preds = %123
  br label %143

130:                                              ; preds = %123
  br label %143

131:                                              ; preds = %123
  br label %143

132:                                              ; preds = %123
  %133 = extractelement <4 x float> %10, i64 2
  %134 = fsub contract float 1.000000e+00, %133
  br label %143

135:                                              ; preds = %123
  %136 = extractelement <4 x float> %10, i64 2
  br label %143

137:                                              ; preds = %123
  br label %143

138:                                              ; preds = %123
  br label %143

139:                                              ; preds = %123
  %140 = fsub contract float 1.000000e+00, %128
  br label %143

141:                                              ; preds = %123
  br label %143

142:                                              ; preds = %123
  br label %143

143:                                              ; preds = %142, %141, %139, %138, %137, %135, %132, %131, %130, %129, %123
  %144 = phi float [ 1.000000e+00, %142 ], [ %15, %129 ], [ %128, %141 ], [ %140, %139 ], [ %11, %138 ], [ %16, %137 ], [ %136, %135 ], [ %134, %132 ], [ %12, %131 ], [ %13, %130 ], [ 0.000000e+00, %123 ]
  %145 = fmul contract float %128, %144
  %146 = extractelement <4 x float> %10, i64 2
  switch i8 %20, label %158 [
    i8 0, label %159
    i8 2, label %157
    i8 3, label %155
    i8 4, label %154
    i8 5, label %153
    i8 6, label %152
    i8 7, label %150
    i8 8, label %149
    i8 9, label %148
    i8 10, label %147
  ]

147:                                              ; preds = %143
  br label %159

148:                                              ; preds = %143
  br label %159

149:                                              ; preds = %143
  br label %159

150:                                              ; preds = %143
  %151 = fsub contract float 1.000000e+00, %146
  br label %159

152:                                              ; preds = %143
  br label %159

153:                                              ; preds = %143
  br label %159

154:                                              ; preds = %143
  br label %159

155:                                              ; preds = %143
  %156 = fsub contract float 1.000000e+00, %128
  br label %159

157:                                              ; preds = %143
  br label %159

158:                                              ; preds = %143
  br label %159

159:                                              ; preds = %158, %157, %155, %154, %153, %152, %150, %149, %148, %147, %143
  %160 = phi float [ 1.000000e+00, %158 ], [ %15, %147 ], [ %128, %157 ], [ %156, %155 ], [ %11, %154 ], [ %16, %153 ], [ %146, %152 ], [ %151, %150 ], [ %12, %149 ], [ %13, %148 ], [ 0.000000e+00, %143 ]
  %161 = fmul contract float %146, %160
  %162 = fadd contract float %145, %161
  %163 = insertelement <4 x float> %127, float %162, i64 2
  %164 = lshr i32 %6, 16
  %165 = lshr i32 %6, 24
  %166 = extractelement <4 x float> %2, i64 3
  %167 = trunc i32 %164 to i8
  switch i8 %167, label %34 [
    i8 0, label %35
    i8 2, label %33
    i8 3, label %31
    i8 4, label %30
    i8 5, label %29
    i8 6, label %27
    i8 7, label %24
    i8 8, label %23
    i8 9, label %22
  ]

168:                                              ; preds = %3, %52
  %169 = phi <4 x float> [ %56, %52 ], [ %2, %3 ]
  store <4 x float> %169, ptr %8, align 16, !tbaa !45
  ret void
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(write, argmem: readwrite, inaccessiblemem: none, target_mem: none) uwtable
define hidden void @rdna4_px_end(ptr nofree noundef readonly captures(none) %0, ptr nofree noundef readonly captures(none) %1) local_unnamed_addr #7 {
  %3 = getelementptr inbounds nuw i8, ptr %1, i64 32
  %4 = load i32, ptr %3, align 16, !tbaa !46
  %5 = icmp eq i32 %4, 0
  br i1 %5, label %65, label %6

6:                                                ; preds = %2
  %7 = getelementptr inbounds nuw i8, ptr %1, i64 16
  %8 = load <4 x float>, ptr %7, align 16, !tbaa !45
  %9 = getelementptr inbounds nuw i8, ptr %0, i64 44
  %10 = load i32, ptr %9, align 4, !tbaa !23
  %11 = extractelement <4 x float> %8, i64 0
  %12 = fcmp contract olt float %11, 0.000000e+00
  %13 = fcmp contract ogt float %11, 1.000000e+00
  %14 = select contract i1 %13, float 1.000000e+00, float %11
  %15 = fmul contract float %14, 2.550000e+02
  %16 = fadd contract float %15, 5.000000e-01
  %17 = select i1 %12, float 5.000000e-01, float %16
  %18 = fptoui float %17 to i32
  %19 = extractelement <4 x float> %8, i64 1
  %20 = fcmp contract olt float %19, 0.000000e+00
  %21 = fcmp contract ogt float %19, 1.000000e+00
  %22 = select contract i1 %21, float 1.000000e+00, float %19
  %23 = fmul contract float %22, 2.550000e+02
  %24 = fadd contract float %23, 5.000000e-01
  %25 = select i1 %20, float 5.000000e-01, float %24
  %26 = fptoui float %25 to i32
  %27 = extractelement <4 x float> %8, i64 2
  %28 = fcmp contract olt float %27, 0.000000e+00
  %29 = fcmp contract ogt float %27, 1.000000e+00
  %30 = select contract i1 %29, float 1.000000e+00, float %27
  %31 = fmul contract float %30, 2.550000e+02
  %32 = fadd contract float %31, 5.000000e-01
  %33 = select i1 %28, float 5.000000e-01, float %32
  %34 = fptoui float %33 to i32
  %35 = extractelement <4 x float> %8, i64 3
  %36 = fcmp contract olt float %35, 0.000000e+00
  %37 = fcmp contract ogt float %35, 1.000000e+00
  %38 = select contract i1 %37, float 1.000000e+00, float %35
  %39 = fmul contract float %38, 2.550000e+02
  %40 = fadd contract float %39, 5.000000e-01
  %41 = select i1 %36, float 5.000000e-01, float %40
  %42 = fptoui float %41 to i32
  %43 = and i32 %10, -2
  %44 = icmp eq i32 %43, 80
  %45 = shl i32 %26, 8
  %46 = shl i32 %42, 24
  %47 = select i1 %44, i32 %18, i32 %34
  %48 = select i1 %44, i32 %34, i32 %18
  %49 = shl i32 %47, 16
  %50 = or i32 %46, %45
  %51 = or i32 %50, %48
  %52 = or i32 %51, %49
  %53 = getelementptr inbounds nuw i8, ptr %0, i64 24
  %54 = load ptr addrspace(1), ptr %53, align 8, !tbaa !19
  %55 = getelementptr inbounds nuw i8, ptr %1, i64 4
  %56 = load i32, ptr %55, align 4, !tbaa !44
  %57 = getelementptr inbounds nuw i8, ptr %0, i64 32
  %58 = load i32, ptr %57, align 8, !tbaa !20
  %59 = lshr i32 %58, 2
  %60 = mul i32 %59, %56
  %61 = load i32, ptr %1, align 16, !tbaa !42
  %62 = add i32 %60, %61
  %63 = zext i32 %62 to i64
  %64 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %54, i64 %63
  store i32 %52, ptr addrspace(1) %64, align 4, !tbaa !40
  br label %65

65:                                               ; preds = %6, %2
  ret void
}

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare i32 @llvm.smin.i32(i32, i32) #8

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare i32 @llvm.smax.i32(i32, i32) #8

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare i32 @llvm.usub.sat.i32(i32, i32) #8

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare i32 @llvm.umin.i32(i32, i32) #8

attributes #0 = { alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: write) uwtable "amdgpu-no-cluster-id-x" "amdgpu-no-cluster-id-y" "amdgpu-no-cluster-id-z" "amdgpu-no-completion-action" "amdgpu-no-default-queue" "amdgpu-no-dispatch-id" "amdgpu-no-dispatch-ptr" "amdgpu-no-flat-scratch-init" "amdgpu-no-heap-ptr" "amdgpu-no-hostcall-ptr" "amdgpu-no-implicitarg-ptr" "amdgpu-no-lds-kernel-id" "amdgpu-no-multigrid-sync-arg" "amdgpu-no-queue-ptr" "amdgpu-no-workgroup-id-x" "amdgpu-no-workgroup-id-y" "amdgpu-no-workgroup-id-z" "amdgpu-no-workitem-id-x" "amdgpu-no-workitem-id-y" "amdgpu-no-workitem-id-z" "amdgpu-no-wwm" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="gfx1201" }
attributes #1 = { alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(read, inaccessiblemem: none, target_mem: none) uwtable "amdgpu-no-cluster-id-x" "amdgpu-no-cluster-id-y" "amdgpu-no-cluster-id-z" "amdgpu-no-completion-action" "amdgpu-no-default-queue" "amdgpu-no-dispatch-id" "amdgpu-no-dispatch-ptr" "amdgpu-no-flat-scratch-init" "amdgpu-no-heap-ptr" "amdgpu-no-hostcall-ptr" "amdgpu-no-implicitarg-ptr" "amdgpu-no-lds-kernel-id" "amdgpu-no-multigrid-sync-arg" "amdgpu-no-queue-ptr" "amdgpu-no-workgroup-id-x" "amdgpu-no-workgroup-id-y" "amdgpu-no-workgroup-id-z" "amdgpu-no-workitem-id-x" "amdgpu-no-workitem-id-y" "amdgpu-no-workitem-id-z" "amdgpu-no-wwm" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="gfx1201" }
attributes #2 = { mustprogress nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }
attributes #3 = { alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(read, argmem: readwrite, inaccessiblemem: none, target_mem: none) uwtable "amdgpu-no-cluster-id-x" "amdgpu-no-cluster-id-y" "amdgpu-no-cluster-id-z" "amdgpu-no-completion-action" "amdgpu-no-default-queue" "amdgpu-no-dispatch-id" "amdgpu-no-dispatch-ptr" "amdgpu-no-flat-scratch-init" "amdgpu-no-heap-ptr" "amdgpu-no-hostcall-ptr" "amdgpu-no-implicitarg-ptr" "amdgpu-no-lds-kernel-id" "amdgpu-no-multigrid-sync-arg" "amdgpu-no-queue-ptr" "amdgpu-no-workgroup-id-x" "amdgpu-no-workgroup-id-y" "amdgpu-no-workgroup-id-z" "amdgpu-no-workitem-id-x" "amdgpu-no-workitem-id-y" "amdgpu-no-workitem-id-z" "amdgpu-no-wwm" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="gfx1201" }
attributes #4 = { alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: read) uwtable "amdgpu-no-cluster-id-x" "amdgpu-no-cluster-id-y" "amdgpu-no-cluster-id-z" "amdgpu-no-completion-action" "amdgpu-no-default-queue" "amdgpu-no-dispatch-id" "amdgpu-no-dispatch-ptr" "amdgpu-no-flat-scratch-init" "amdgpu-no-heap-ptr" "amdgpu-no-hostcall-ptr" "amdgpu-no-implicitarg-ptr" "amdgpu-no-lds-kernel-id" "amdgpu-no-multigrid-sync-arg" "amdgpu-no-queue-ptr" "amdgpu-no-workgroup-id-x" "amdgpu-no-workgroup-id-y" "amdgpu-no-workgroup-id-z" "amdgpu-no-workitem-id-x" "amdgpu-no-workitem-id-y" "amdgpu-no-workitem-id-z" "amdgpu-no-wwm" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="gfx1201" }
attributes #5 = { alwaysinline nofree norecurse nosync nounwind memory(read, inaccessiblemem: none, target_mem: none) uwtable "amdgpu-no-cluster-id-x" "amdgpu-no-cluster-id-y" "amdgpu-no-cluster-id-z" "amdgpu-no-completion-action" "amdgpu-no-default-queue" "amdgpu-no-dispatch-id" "amdgpu-no-dispatch-ptr" "amdgpu-no-flat-scratch-init" "amdgpu-no-heap-ptr" "amdgpu-no-hostcall-ptr" "amdgpu-no-implicitarg-ptr" "amdgpu-no-lds-kernel-id" "amdgpu-no-multigrid-sync-arg" "amdgpu-no-queue-ptr" "amdgpu-no-workgroup-id-x" "amdgpu-no-workgroup-id-y" "amdgpu-no-workgroup-id-z" "amdgpu-no-workitem-id-x" "amdgpu-no-workitem-id-y" "amdgpu-no-workitem-id-z" "amdgpu-no-wwm" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="gfx1201" }
attributes #6 = { alwaysinline nofree norecurse nosync nounwind memory(argmem: readwrite) uwtable "amdgpu-no-cluster-id-x" "amdgpu-no-cluster-id-y" "amdgpu-no-cluster-id-z" "amdgpu-no-completion-action" "amdgpu-no-default-queue" "amdgpu-no-dispatch-id" "amdgpu-no-dispatch-ptr" "amdgpu-no-flat-scratch-init" "amdgpu-no-heap-ptr" "amdgpu-no-hostcall-ptr" "amdgpu-no-implicitarg-ptr" "amdgpu-no-lds-kernel-id" "amdgpu-no-multigrid-sync-arg" "amdgpu-no-queue-ptr" "amdgpu-no-workgroup-id-x" "amdgpu-no-workgroup-id-y" "amdgpu-no-workgroup-id-z" "amdgpu-no-workitem-id-x" "amdgpu-no-workitem-id-y" "amdgpu-no-workitem-id-z" "amdgpu-no-wwm" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="gfx1201" }
attributes #7 = { alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(write, argmem: readwrite, inaccessiblemem: none, target_mem: none) uwtable "amdgpu-no-cluster-id-x" "amdgpu-no-cluster-id-y" "amdgpu-no-cluster-id-z" "amdgpu-no-completion-action" "amdgpu-no-default-queue" "amdgpu-no-dispatch-id" "amdgpu-no-dispatch-ptr" "amdgpu-no-flat-scratch-init" "amdgpu-no-heap-ptr" "amdgpu-no-hostcall-ptr" "amdgpu-no-implicitarg-ptr" "amdgpu-no-lds-kernel-id" "amdgpu-no-multigrid-sync-arg" "amdgpu-no-queue-ptr" "amdgpu-no-workgroup-id-x" "amdgpu-no-workgroup-id-y" "amdgpu-no-workgroup-id-z" "amdgpu-no-workitem-id-x" "amdgpu-no-workitem-id-y" "amdgpu-no-workitem-id-z" "amdgpu-no-wwm" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="gfx1201" }
attributes #8 = { nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }

!llvm.module.flags = !{!0, !1, !2}
!llvm.ident = !{!3}
!llvm.errno.tbaa = !{!4}

!0 = !{i32 1, !"amdhsa_code_object_version", i32 600}
!1 = !{i32 8, !"PIC Level", i32 2}
!2 = !{i32 7, !"uwtable", i32 2}
!3 = !{!"clang version 23.1.4"}
!4 = !{!5, !6, i64 0}
!5 = !{!"__libc_errno", !6, i64 0}
!6 = !{!"int", !7, i64 0}
!7 = !{!"omnipotent char", !8, i64 0}
!8 = !{!"Simple C/C++ TBAA"}
!9 = !{!10, !11, i64 0}
!10 = !{!"rdna4_draw", !11, i64 0, !6, i64 8, !6, i64 12, !6, i64 16, !6, i64 20, !13, i64 24, !6, i64 32, !6, i64 36, !6, i64 40, !6, i64 44, !14, i64 48, !14, i64 52, !14, i64 56, !14, i64 60, !6, i64 64, !6, i64 68, !6, i64 72, !6, i64 76, !6, i64 80}
!11 = !{!"p1 float", !12, i64 0}
!12 = !{!"any pointer", !7, i64 0}
!13 = !{!"p1 int", !12, i64 0}
!14 = !{!"float", !7, i64 0}
!15 = !{!10, !6, i64 8}
!16 = !{!10, !6, i64 12}
!17 = !{!10, !6, i64 16}
!18 = !{!10, !6, i64 20}
!19 = !{!10, !13, i64 24}
!20 = !{!10, !6, i64 32}
!21 = !{!10, !6, i64 36}
!22 = !{!10, !6, i64 40}
!23 = !{!10, !6, i64 44}
!24 = !{!10, !14, i64 48}
!25 = !{!10, !14, i64 52}
!26 = !{!10, !14, i64 56}
!27 = !{!10, !14, i64 60}
!28 = !{!10, !6, i64 64}
!29 = !{!10, !6, i64 68}
!30 = !{!10, !6, i64 72}
!31 = !{!10, !6, i64 76}
!32 = !{!10, !6, i64 80}
!33 = !{!34, !6, i64 0}
!34 = !{!"rdna4_sampler", !6, i64 0}
!35 = !{!36, !6, i64 8}
!36 = !{!"rdna4_texture", !13, i64 0, !6, i64 8, !6, i64 12, !6, i64 16, !6, i64 20}
!37 = !{!36, !6, i64 12}
!38 = !{!36, !13, i64 0}
!39 = !{!36, !6, i64 16}
!40 = !{!6, !6, i64 0}
!41 = !{!36, !6, i64 20}
!42 = !{!43, !6, i64 0}
!43 = !{!"rdna4_px", !6, i64 0, !6, i64 4, !7, i64 16, !6, i64 32}
!44 = !{!43, !6, i64 4}
!45 = !{!7, !7, i64 0}
!46 = !{!43, !6, i64 32}
!47 = !{!11, !11, i64 0}
!48 = !{!14, !14, i64 0}
!49 = !{!50, !14, i64 48}
!50 = !{!"rdna4_tri", !7, i64 0, !7, i64 24, !7, i64 36, !14, i64 48, !14, i64 52, !6, i64 56}
!51 = !{!50, !14, i64 52}
!52 = !{!50, !6, i64 56}
!53 = distinct !{!53, !54}
!54 = !{!"llvm.loop.mustprogress"}

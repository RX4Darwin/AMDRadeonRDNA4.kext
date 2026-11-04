; ModuleID = 'rdna4raster.c'
source_filename = "rdna4raster.c"
target datalayout = "e-m:e-p:64:64-p1:64:64-p2:32:32-p3:32:32-p4:64:64-p5:32:32-p6:32:32-p7:160:256:256:32-p8:128:128:128:48-p9:192:256:256:32-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-v2048:2048-n32:64-S32-A5-G1-ni:7:8:9"
target triple = "amdgcn-amd-amdhsa"

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: write) uwtable
define hidden void @rdna4_draw_init(ptr nofree noundef writeonly captures(none) initializes((0, 68)) %0, ptr addrspace(1) noundef %1, i32 noundef %2, i32 noundef %3, i32 noundef %4, i32 noundef %5, ptr addrspace(1) noundef %6, i32 noundef %7, i32 noundef %8, i32 noundef %9, i32 noundef %10, float noundef %11, float noundef %12, float noundef %13, float noundef %14, i32 noundef %15) local_unnamed_addr #0 {
  store ptr addrspace(1) %1, ptr %0, align 8, !tbaa !9
  %17 = getelementptr inbounds nuw i8, ptr %0, i64 8
  store i32 %2, ptr %17, align 8, !tbaa !15
  %18 = getelementptr inbounds nuw i8, ptr %0, i64 12
  store i32 %3, ptr %18, align 4, !tbaa !16
  %19 = getelementptr inbounds nuw i8, ptr %0, i64 16
  store i32 %4, ptr %19, align 8, !tbaa !17
  %20 = getelementptr inbounds nuw i8, ptr %0, i64 20
  store i32 %5, ptr %20, align 4, !tbaa !18
  %21 = getelementptr inbounds nuw i8, ptr %0, i64 24
  store ptr addrspace(1) %6, ptr %21, align 8, !tbaa !19
  %22 = getelementptr inbounds nuw i8, ptr %0, i64 32
  store i32 %7, ptr %22, align 8, !tbaa !20
  %23 = getelementptr inbounds nuw i8, ptr %0, i64 36
  store i32 %8, ptr %23, align 4, !tbaa !21
  %24 = getelementptr inbounds nuw i8, ptr %0, i64 40
  store i32 %9, ptr %24, align 8, !tbaa !22
  %25 = getelementptr inbounds nuw i8, ptr %0, i64 44
  store i32 %10, ptr %25, align 4, !tbaa !23
  %26 = getelementptr inbounds nuw i8, ptr %0, i64 48
  store float %11, ptr %26, align 8, !tbaa !24
  %27 = getelementptr inbounds nuw i8, ptr %0, i64 52
  store float %12, ptr %27, align 4, !tbaa !25
  %28 = getelementptr inbounds nuw i8, ptr %0, i64 56
  store float %13, ptr %28, align 8, !tbaa !26
  %29 = getelementptr inbounds nuw i8, ptr %0, i64 60
  store float %14, ptr %29, align 4, !tbaa !27
  %30 = getelementptr inbounds nuw i8, ptr %0, i64 64
  store i32 %15, ptr %30, align 8, !tbaa !28
  ret void
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(read, inaccessiblemem: none, target_mem: none) uwtable
define hidden <4 x float> @rdna4_sample_2d(ptr addrspace(1) nofree noundef readonly captures(none) %0, ptr addrspace(4) nofree noundef readonly captures(none) %1, <2 x float> noundef %2) local_unnamed_addr #1 {
  %4 = load i32, ptr addrspace(4) %1, align 4, !tbaa !29
  %5 = lshr i32 %4, 5
  %6 = and i32 %5, 7
  %7 = lshr i32 %4, 8
  %8 = and i32 %7, 7
  %9 = icmp sgt i32 %4, -1
  %10 = extractelement <2 x float> %2, i64 0
  br i1 %9, label %16, label %11

11:                                               ; preds = %3
  %12 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 8
  %13 = load i32, ptr addrspace(1) %12, align 8, !tbaa !31
  %14 = uitofp contract i32 %13 to float
  %15 = fmul contract float %10, %14
  br label %16

16:                                               ; preds = %3, %11
  %17 = phi contract float [ %15, %11 ], [ %10, %3 ]
  %18 = extractelement <2 x float> %2, i64 1
  br i1 %9, label %24, label %19

19:                                               ; preds = %16
  %20 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 12
  %21 = load i32, ptr addrspace(1) %20, align 4, !tbaa !33
  %22 = uitofp contract i32 %21 to float
  %23 = fmul contract float %18, %22
  br label %24

24:                                               ; preds = %16, %19
  %25 = phi contract float [ %23, %19 ], [ %18, %16 ]
  %26 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 8
  %27 = load i32, ptr addrspace(1) %26, align 8, !tbaa !31
  %28 = icmp eq i32 %27, 0
  br i1 %28, label %462, label %29

29:                                               ; preds = %24
  %30 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 12
  %31 = load i32, ptr addrspace(1) %30, align 4, !tbaa !33
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
  %106 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !34
  %107 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %108 = load i32, ptr addrspace(1) %107, align 8, !tbaa !35
  %109 = lshr i32 %108, 2
  %110 = mul i32 %109, %102
  %111 = add i32 %110, %69
  %112 = zext i32 %111 to i64
  %113 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %106, i64 %112
  %114 = load i32, ptr addrspace(1) %113, align 4, !tbaa !36
  %115 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %116 = load i32, ptr addrspace(1) %115, align 4, !tbaa !37
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
  %282 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !34
  %283 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %284 = load i32, ptr addrspace(1) %283, align 8, !tbaa !35
  %285 = lshr i32 %284, 2
  %286 = mul i32 %285, %246
  %287 = add i32 %286, %183
  %288 = zext i32 %287 to i64
  %289 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %282, i64 %288
  %290 = load i32, ptr addrspace(1) %289, align 4, !tbaa !36
  %291 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %292 = load i32, ptr addrspace(1) %291, align 4, !tbaa !37
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
  %327 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !34
  %328 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %329 = load i32, ptr addrspace(1) %328, align 8, !tbaa !35
  %330 = lshr i32 %329, 2
  %331 = mul i32 %330, %246
  %332 = add i32 %331, %215
  %333 = zext i32 %332 to i64
  %334 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %327, i64 %333
  %335 = load i32, ptr addrspace(1) %334, align 4, !tbaa !36
  %336 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %337 = load i32, ptr addrspace(1) %336, align 4, !tbaa !37
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
  %372 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !34
  %373 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %374 = load i32, ptr addrspace(1) %373, align 8, !tbaa !35
  %375 = lshr i32 %374, 2
  %376 = mul i32 %375, %278
  %377 = add i32 %376, %183
  %378 = zext i32 %377 to i64
  %379 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %372, i64 %378
  %380 = load i32, ptr addrspace(1) %379, align 4, !tbaa !36
  %381 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %382 = load i32, ptr addrspace(1) %381, align 4, !tbaa !37
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
  %414 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !34
  %415 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %416 = load i32, ptr addrspace(1) %415, align 8, !tbaa !35
  %417 = lshr i32 %416, 2
  %418 = mul i32 %417, %278
  %419 = add i32 %418, %215
  %420 = zext i32 %419 to i64
  %421 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %414, i64 %420
  %422 = load i32, ptr addrspace(1) %421, align 4, !tbaa !36
  %423 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %424 = load i32, ptr addrspace(1) %423, align 4, !tbaa !37
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
define hidden range(i32 0, 2) i32 @rdna4_px_begin(ptr nofree noundef readonly captures(none) %0, i32 noundef %1, i32 noundef %2, ptr nofree noundef writeonly captures(none) %3) local_unnamed_addr #3 {
  %5 = getelementptr inbounds nuw i8, ptr %0, i64 36
  %6 = load i32, ptr %5, align 4, !tbaa !21
  %7 = icmp ult i32 %1, %6
  br i1 %7, label %8, label %53

8:                                                ; preds = %4
  %9 = getelementptr inbounds nuw i8, ptr %0, i64 40
  %10 = load i32, ptr %9, align 8, !tbaa !22
  %11 = icmp ult i32 %2, %10
  br i1 %11, label %12, label %53

12:                                               ; preds = %8
  store i32 %1, ptr %3, align 16, !tbaa !38
  %13 = getelementptr inbounds nuw i8, ptr %3, i64 4
  store i32 %2, ptr %13, align 4, !tbaa !40
  %14 = getelementptr inbounds nuw i8, ptr %0, i64 24
  %15 = load ptr addrspace(1), ptr %14, align 8, !tbaa !19
  %16 = getelementptr inbounds nuw i8, ptr %0, i64 32
  %17 = load i32, ptr %16, align 8, !tbaa !20
  %18 = lshr i32 %17, 2
  %19 = mul i32 %18, %2
  %20 = add i32 %19, %1
  %21 = zext i32 %20 to i64
  %22 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %15, i64 %21
  %23 = load i32, ptr addrspace(1) %22, align 4, !tbaa !36
  %24 = getelementptr inbounds nuw i8, ptr %0, i64 44
  %25 = load i32, ptr %24, align 4, !tbaa !23
  %26 = and i32 %23, 255
  %27 = uitofp contract nneg i32 %26 to float
  %28 = fdiv contract float %27, 2.550000e+02
  %29 = lshr i32 %23, 8
  %30 = and i32 %29, 255
  %31 = uitofp contract nneg i32 %30 to float
  %32 = fdiv contract float %31, 2.550000e+02
  %33 = lshr i32 %23, 16
  %34 = and i32 %33, 255
  %35 = uitofp contract nneg i32 %34 to float
  %36 = fdiv contract float %35, 2.550000e+02
  %37 = lshr i32 %23, 24
  %38 = uitofp contract nneg i32 %37 to float
  %39 = fdiv contract float %38, 2.550000e+02
  %40 = and i32 %25, -2
  %41 = icmp eq i32 %40, 80
  %42 = insertelement <4 x float> poison, float %36, i64 0
  %43 = insertelement <4 x float> %42, float %32, i64 1
  %44 = insertelement <4 x float> %43, float %28, i64 2
  %45 = insertelement <4 x float> %44, float %39, i64 3
  %46 = insertelement <4 x float> poison, float %28, i64 0
  %47 = insertelement <4 x float> %46, float %32, i64 1
  %48 = insertelement <4 x float> %47, float %36, i64 2
  %49 = insertelement <4 x float> %48, float %39, i64 3
  %50 = select i1 %41, <4 x float> %45, <4 x float> %49
  %51 = getelementptr inbounds nuw i8, ptr %3, i64 16
  store <4 x float> %50, ptr %51, align 16, !tbaa !41
  %52 = getelementptr inbounds nuw i8, ptr %3, i64 32
  store i32 0, ptr %52, align 16, !tbaa !42
  br label %53

53:                                               ; preds = %4, %8, %12
  %54 = phi i32 [ 1, %12 ], [ 0, %8 ], [ 0, %4 ]
  ret i32 %54
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
define hidden range(i32 0, 2) i32 @rdna4_tri_cover(ptr nofree noundef readonly captures(none) %0, ptr nofree noundef readonly captures(none) %1, i32 noundef %2, ptr nofree noundef writeonly captures(none) initializes((0, 24)) %3) local_unnamed_addr #3 {
  %5 = mul i32 %2, 3
  %6 = add i32 %5, 1
  %7 = load i32, ptr %1, align 16, !tbaa !38
  %8 = uitofp contract i32 %7 to float
  %9 = fadd contract float %8, 5.000000e-01
  %10 = getelementptr inbounds nuw i8, ptr %1, i64 4
  %11 = load i32, ptr %10, align 4, !tbaa !40
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
  store ptr addrspace(1) %29, ptr %3, align 8, !tbaa !43
  %30 = load ptr addrspace(1), ptr %0, align 8, !tbaa !9
  %31 = mul i32 %21, %26
  %32 = zext i32 %31 to i64
  %33 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %30, i64 %32
  %34 = getelementptr inbounds nuw i8, ptr %3, i64 8
  store ptr addrspace(1) %33, ptr %34, align 8, !tbaa !43
  %35 = load ptr addrspace(1), ptr %0, align 8, !tbaa !9
  %36 = mul i32 %23, %26
  %37 = zext i32 %36 to i64
  %38 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %35, i64 %37
  %39 = getelementptr inbounds nuw i8, ptr %3, i64 16
  store ptr addrspace(1) %38, ptr %39, align 8, !tbaa !43
  %40 = getelementptr inbounds nuw i8, ptr %0, i64 12
  %41 = load i32, ptr %40, align 4, !tbaa !16
  %42 = add i32 %41, 3
  %43 = zext i32 %42 to i64
  %44 = zext i32 %41 to i64
  %45 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %29, i64 %43
  %46 = load float, ptr addrspace(1) %45, align 4, !tbaa !44
  %47 = fcmp contract ugt float %46, 0.000000e+00
  br i1 %47, label %48, label %186

48:                                               ; preds = %4
  %49 = getelementptr inbounds nuw i8, ptr %0, i64 60
  %50 = getelementptr inbounds nuw i8, ptr %0, i64 52
  %51 = getelementptr inbounds nuw i8, ptr %0, i64 56
  %52 = getelementptr inbounds nuw i8, ptr %0, i64 48
  %53 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %29, i64 %44
  %54 = getelementptr inbounds nuw i8, ptr addrspace(1) %53, i64 12
  %55 = load float, ptr addrspace(1) %54, align 4, !tbaa !44
  %56 = fdiv contract float 1.000000e+00, %55
  %57 = load float, ptr %52, align 8, !tbaa !24
  %58 = load float, ptr addrspace(1) %53, align 4, !tbaa !44
  %59 = fmul contract float %56, %58
  %60 = fmul contract float %59, 5.000000e-01
  %61 = fadd contract float %60, 5.000000e-01
  %62 = load float, ptr %51, align 8, !tbaa !26
  %63 = fmul contract float %62, %61
  %64 = fadd contract float %57, %63
  %65 = load float, ptr %50, align 4, !tbaa !25
  %66 = getelementptr inbounds nuw i8, ptr addrspace(1) %53, i64 4
  %67 = load float, ptr addrspace(1) %66, align 4, !tbaa !44
  %68 = fmul contract float %56, %67
  %69 = fmul contract float %68, 5.000000e-01
  %70 = fsub contract float 5.000000e-01, %69
  %71 = load float, ptr %49, align 4, !tbaa !27
  %72 = fmul contract float %71, %70
  %73 = fadd contract float %65, %72
  %74 = getelementptr inbounds nuw i8, ptr addrspace(1) %53, i64 8
  %75 = load float, ptr addrspace(1) %74, align 4, !tbaa !44
  %76 = fmul contract float %56, %75
  %77 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %33, i64 %43
  %78 = load float, ptr addrspace(1) %77, align 4, !tbaa !44
  %79 = fcmp contract ugt float %78, 0.000000e+00
  br i1 %79, label %80, label %186

80:                                               ; preds = %48
  %81 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %33, i64 %44
  %82 = getelementptr inbounds nuw i8, ptr addrspace(1) %81, i64 12
  %83 = load float, ptr addrspace(1) %82, align 4, !tbaa !44
  %84 = fdiv contract float 1.000000e+00, %83
  %85 = load float, ptr addrspace(1) %81, align 4, !tbaa !44
  %86 = fmul contract float %84, %85
  %87 = fmul contract float %86, 5.000000e-01
  %88 = fadd contract float %87, 5.000000e-01
  %89 = fmul contract float %62, %88
  %90 = fadd contract float %57, %89
  %91 = getelementptr inbounds nuw i8, ptr addrspace(1) %81, i64 4
  %92 = load float, ptr addrspace(1) %91, align 4, !tbaa !44
  %93 = fmul contract float %84, %92
  %94 = fmul contract float %93, 5.000000e-01
  %95 = fsub contract float 5.000000e-01, %94
  %96 = fmul contract float %71, %95
  %97 = fadd contract float %65, %96
  %98 = getelementptr inbounds nuw i8, ptr addrspace(1) %81, i64 8
  %99 = load float, ptr addrspace(1) %98, align 4, !tbaa !44
  %100 = fmul contract float %84, %99
  %101 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %38, i64 %43
  %102 = load float, ptr addrspace(1) %101, align 4, !tbaa !44
  %103 = fcmp contract ugt float %102, 0.000000e+00
  br i1 %103, label %104, label %186

104:                                              ; preds = %80
  %105 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %38, i64 %44
  %106 = getelementptr inbounds nuw i8, ptr addrspace(1) %105, i64 12
  %107 = load float, ptr addrspace(1) %106, align 4, !tbaa !44
  %108 = fdiv contract float 1.000000e+00, %107
  %109 = load float, ptr addrspace(1) %105, align 4, !tbaa !44
  %110 = fmul contract float %108, %109
  %111 = fmul contract float %110, 5.000000e-01
  %112 = fadd contract float %111, 5.000000e-01
  %113 = fmul contract float %62, %112
  %114 = fadd contract float %57, %113
  %115 = getelementptr inbounds nuw i8, ptr addrspace(1) %105, i64 4
  %116 = load float, ptr addrspace(1) %115, align 4, !tbaa !44
  %117 = fmul contract float %108, %116
  %118 = fmul contract float %117, 5.000000e-01
  %119 = fsub contract float 5.000000e-01, %118
  %120 = fmul contract float %71, %119
  %121 = fadd contract float %65, %120
  %122 = getelementptr inbounds nuw i8, ptr addrspace(1) %105, i64 8
  %123 = load float, ptr addrspace(1) %122, align 4, !tbaa !44
  %124 = fmul contract float %108, %123
  %125 = fsub contract float %90, %64
  %126 = fsub contract float %121, %73
  %127 = fmul contract float %126, %125
  %128 = fsub contract float %114, %64
  %129 = fsub contract float %97, %73
  %130 = fmul contract float %129, %128
  %131 = fsub contract float %127, %130
  %132 = fcmp contract oeq float %131, 0.000000e+00
  br i1 %132, label %186, label %133

133:                                              ; preds = %104
  %134 = fsub contract float %114, %90
  %135 = fsub contract float %13, %97
  %136 = fmul contract float %135, %134
  %137 = fsub contract float %121, %97
  %138 = fsub contract float %9, %90
  %139 = fmul contract float %137, %138
  %140 = fsub contract float %136, %139
  %141 = fsub contract float %13, %73
  %142 = fmul contract float %141, %125
  %143 = fsub contract float %9, %64
  %144 = fmul contract float %129, %143
  %145 = fsub contract float %142, %144
  %146 = fdiv contract float %140, %131
  %147 = fcmp contract olt float %146, 0.000000e+00
  br i1 %147, label %186, label %148

148:                                              ; preds = %133
  %149 = fsub contract float %13, %121
  %150 = fsub contract float %64, %114
  %151 = fmul contract float %149, %150
  %152 = fsub contract float %73, %121
  %153 = fsub contract float %9, %114
  %154 = fmul contract float %152, %153
  %155 = fsub contract float %151, %154
  %156 = fdiv contract float %155, %131
  %157 = fcmp contract olt float %156, 0.000000e+00
  br i1 %157, label %186, label %158

158:                                              ; preds = %148
  %159 = fdiv contract float %145, %131
  %160 = fcmp contract olt float %159, 0.000000e+00
  br i1 %160, label %186, label %161

161:                                              ; preds = %158
  %162 = fmul contract float %56, %146
  %163 = fmul contract float %84, %156
  %164 = fadd contract float %162, %163
  %165 = fmul contract float %108, %159
  %166 = fadd contract float %165, %164
  %167 = getelementptr inbounds nuw i8, ptr %3, i64 36
  %168 = getelementptr inbounds nuw i8, ptr %3, i64 24
  store float %146, ptr %167, align 4, !tbaa !44
  %169 = fdiv contract float %162, %166
  store float %169, ptr %168, align 8, !tbaa !44
  %170 = getelementptr inbounds nuw i8, ptr %3, i64 40
  store float %156, ptr %170, align 8, !tbaa !44
  %171 = fdiv contract float %163, %166
  %172 = getelementptr inbounds nuw i8, ptr %3, i64 28
  store float %171, ptr %172, align 4, !tbaa !44
  %173 = getelementptr inbounds nuw i8, ptr %3, i64 44
  store float %159, ptr %173, align 4, !tbaa !44
  %174 = fdiv contract float %165, %166
  %175 = getelementptr inbounds nuw i8, ptr %3, i64 32
  store float %174, ptr %175, align 8, !tbaa !44
  %176 = fmul contract float %76, %146
  %177 = fmul contract float %100, %156
  %178 = fadd contract float %176, %177
  %179 = fmul contract float %124, %159
  %180 = fadd contract float %179, %178
  %181 = getelementptr inbounds nuw i8, ptr %3, i64 48
  store float %180, ptr %181, align 8, !tbaa !45
  %182 = getelementptr inbounds nuw i8, ptr %3, i64 52
  store float %166, ptr %182, align 4, !tbaa !47
  %183 = fcmp contract olt float %131, 0.000000e+00
  %184 = zext i1 %183 to i32
  %185 = getelementptr inbounds nuw i8, ptr %3, i64 56
  store i32 %184, ptr %185, align 8, !tbaa !48
  br label %186

186:                                              ; preds = %4, %48, %80, %133, %148, %158, %104, %161
  %187 = phi i32 [ 0, %104 ], [ 1, %161 ], [ 0, %133 ], [ 0, %158 ], [ 0, %148 ], [ 0, %80 ], [ 0, %48 ], [ 0, %4 ]
  ret i32 %187
}

; Function Attrs: alwaysinline nofree norecurse nosync nounwind memory(read, inaccessiblemem: none, target_mem: none) uwtable
define hidden <4 x float> @rdna4_interp(ptr nofree noundef readonly captures(none) %0, i32 noundef %1, i32 noundef %2, i32 noundef %3) local_unnamed_addr #5 {
  %5 = tail call i32 @llvm.umin.i32(i32 %2, i32 4)
  %6 = icmp eq i32 %2, 0
  br i1 %6, label %19, label %7

7:                                                ; preds = %4
  %8 = load ptr addrspace(1), ptr %0, align 8, !tbaa !43
  %9 = getelementptr inbounds nuw i8, ptr %0, i64 8
  %10 = load ptr addrspace(1), ptr %9, align 8, !tbaa !43
  %11 = getelementptr inbounds nuw i8, ptr %0, i64 16
  %12 = load ptr addrspace(1), ptr %11, align 8, !tbaa !43
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
  %27 = load float, ptr addrspace(1) %26, align 4, !tbaa !44
  %28 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %10, i64 %25
  %29 = load float, ptr addrspace(1) %28, align 4, !tbaa !44
  %30 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %12, i64 %25
  %31 = load float, ptr addrspace(1) %30, align 4, !tbaa !44
  switch i32 %3, label %41 [
    i32 2, label %50
    i32 1, label %32
  ]

32:                                               ; preds = %21
  %33 = load float, ptr %13, align 4, !tbaa !44
  %34 = fmul contract float %27, %33
  %35 = load float, ptr %14, align 8, !tbaa !44
  %36 = fmul contract float %29, %35
  %37 = fadd contract float %34, %36
  %38 = load float, ptr %15, align 4, !tbaa !44
  %39 = fmul contract float %31, %38
  %40 = fadd contract float %37, %39
  br label %50

41:                                               ; preds = %21
  %42 = load float, ptr %16, align 8, !tbaa !44
  %43 = fmul contract float %27, %42
  %44 = load float, ptr %17, align 4, !tbaa !44
  %45 = fmul contract float %29, %44
  %46 = fadd contract float %43, %45
  %47 = load float, ptr %18, align 8, !tbaa !44
  %48 = fmul contract float %31, %47
  %49 = fadd contract float %46, %48
  br label %50

50:                                               ; preds = %21, %32, %41
  %51 = phi float [ %49, %41 ], [ %40, %32 ], [ %27, %21 ]
  %52 = insertelement <4 x float> %22, float %51, i32 %23
  %53 = add nuw nsw i32 %23, 1
  %54 = icmp eq i32 %53, %5
  br i1 %54, label %19, label %21, !llvm.loop !49
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: read) uwtable
define hidden <4 x float> @rdna4_frag_position(ptr nofree noundef readonly captures(none) %0, ptr nofree noundef readonly captures(none) %1) local_unnamed_addr #4 {
  %3 = load i32, ptr %0, align 16, !tbaa !38
  %4 = uitofp contract i32 %3 to float
  %5 = fadd contract float %4, 5.000000e-01
  %6 = insertelement <4 x float> poison, float %5, i64 0
  %7 = getelementptr inbounds nuw i8, ptr %0, i64 4
  %8 = load i32, ptr %7, align 4, !tbaa !40
  %9 = uitofp contract i32 %8 to float
  %10 = fadd contract float %9, 5.000000e-01
  %11 = insertelement <4 x float> %6, float %10, i64 1
  %12 = getelementptr inbounds nuw i8, ptr %1, i64 48
  %13 = load float, ptr %12, align 8, !tbaa !45
  %14 = insertelement <4 x float> %11, float %13, i64 2
  %15 = getelementptr inbounds nuw i8, ptr %1, i64 52
  %16 = load float, ptr %15, align 4, !tbaa !47
  %17 = insertelement <4 x float> %14, float %16, i64 3
  ret <4 x float> %17
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: read) uwtable
define hidden i32 @rdna4_front_facing(ptr nofree noundef readonly captures(none) %0) local_unnamed_addr #4 {
  %2 = getelementptr inbounds nuw i8, ptr %0, i64 56
  %3 = load i32, ptr %2, align 8, !tbaa !48
  ret i32 %3
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: readwrite) uwtable
define hidden void @rdna4_px_blend(ptr nofree noundef readonly captures(none) %0, ptr nofree noundef captures(none) initializes((32, 36)) %1, <4 x float> noundef %2) local_unnamed_addr #6 {
  %4 = getelementptr inbounds nuw i8, ptr %1, i64 32
  store i32 1, ptr %4, align 16, !tbaa !42
  %5 = getelementptr inbounds nuw i8, ptr %0, i64 64
  %6 = load i32, ptr %5, align 8, !tbaa !28
  %7 = icmp eq i32 %6, 0
  %8 = getelementptr inbounds nuw i8, ptr %1, i64 16
  br i1 %7, label %102, label %9

9:                                                ; preds = %3
  %10 = load <4 x float>, ptr %8, align 16, !tbaa !41
  %11 = extractelement <4 x float> %10, i64 3
  %12 = fsub contract float 1.000000e+00, %11
  %13 = extractelement <4 x float> %2, i64 3
  %14 = fsub contract float 1.000000e+00, %13
  %15 = lshr i32 %6, 8
  %16 = extractelement <4 x float> %2, i64 0
  %17 = trunc i32 %6 to i8
  switch i8 %17, label %22 [
    i8 0, label %23
    i8 4, label %18
    i8 5, label %19
    i8 8, label %20
    i8 9, label %21
  ]

18:                                               ; preds = %9
  br label %23

19:                                               ; preds = %9
  br label %23

20:                                               ; preds = %9
  br label %23

21:                                               ; preds = %9
  br label %23

22:                                               ; preds = %9
  br label %23

23:                                               ; preds = %9, %18, %19, %20, %21, %22
  %24 = phi float [ 1.000000e+00, %22 ], [ %12, %21 ], [ %13, %18 ], [ %14, %19 ], [ %11, %20 ], [ 0.000000e+00, %9 ]
  %25 = fmul contract float %16, %24
  %26 = extractelement <4 x float> %10, i64 0
  %27 = trunc i32 %15 to i8
  switch i8 %27, label %32 [
    i8 0, label %33
    i8 4, label %28
    i8 5, label %29
    i8 8, label %30
    i8 9, label %31
  ]

28:                                               ; preds = %23
  br label %33

29:                                               ; preds = %23
  br label %33

30:                                               ; preds = %23
  br label %33

31:                                               ; preds = %23
  br label %33

32:                                               ; preds = %23
  br label %33

33:                                               ; preds = %23, %28, %29, %30, %31, %32
  %34 = phi float [ 1.000000e+00, %32 ], [ %12, %31 ], [ %13, %28 ], [ %14, %29 ], [ %11, %30 ], [ 0.000000e+00, %23 ]
  %35 = fmul contract float %26, %34
  %36 = fadd contract float %25, %35
  %37 = insertelement <4 x float> poison, float %36, i64 0
  %38 = extractelement <4 x float> %2, i64 1
  switch i8 %17, label %43 [
    i8 0, label %44
    i8 4, label %42
    i8 5, label %41
    i8 8, label %40
    i8 9, label %39
  ]

39:                                               ; preds = %33
  br label %44

40:                                               ; preds = %33
  br label %44

41:                                               ; preds = %33
  br label %44

42:                                               ; preds = %33
  br label %44

43:                                               ; preds = %33
  br label %44

44:                                               ; preds = %43, %42, %41, %40, %39, %33
  %45 = phi float [ 1.000000e+00, %43 ], [ %12, %39 ], [ %13, %42 ], [ %14, %41 ], [ %11, %40 ], [ 0.000000e+00, %33 ]
  %46 = fmul contract float %38, %45
  %47 = extractelement <4 x float> %10, i64 1
  switch i8 %27, label %52 [
    i8 0, label %53
    i8 4, label %51
    i8 5, label %50
    i8 8, label %49
    i8 9, label %48
  ]

48:                                               ; preds = %44
  br label %53

49:                                               ; preds = %44
  br label %53

50:                                               ; preds = %44
  br label %53

51:                                               ; preds = %44
  br label %53

52:                                               ; preds = %44
  br label %53

53:                                               ; preds = %52, %51, %50, %49, %48, %44
  %54 = phi float [ 1.000000e+00, %52 ], [ %12, %48 ], [ %13, %51 ], [ %14, %50 ], [ %11, %49 ], [ 0.000000e+00, %44 ]
  %55 = fmul contract float %47, %54
  %56 = fadd contract float %46, %55
  %57 = insertelement <4 x float> %37, float %56, i64 1
  %58 = extractelement <4 x float> %2, i64 2
  switch i8 %17, label %63 [
    i8 0, label %64
    i8 4, label %62
    i8 5, label %61
    i8 8, label %60
    i8 9, label %59
  ]

59:                                               ; preds = %53
  br label %64

60:                                               ; preds = %53
  br label %64

61:                                               ; preds = %53
  br label %64

62:                                               ; preds = %53
  br label %64

63:                                               ; preds = %53
  br label %64

64:                                               ; preds = %63, %62, %61, %60, %59, %53
  %65 = phi float [ 1.000000e+00, %63 ], [ %12, %59 ], [ %13, %62 ], [ %14, %61 ], [ %11, %60 ], [ 0.000000e+00, %53 ]
  %66 = fmul contract float %58, %65
  %67 = extractelement <4 x float> %10, i64 2
  switch i8 %27, label %72 [
    i8 0, label %73
    i8 4, label %71
    i8 5, label %70
    i8 8, label %69
    i8 9, label %68
  ]

68:                                               ; preds = %64
  br label %73

69:                                               ; preds = %64
  br label %73

70:                                               ; preds = %64
  br label %73

71:                                               ; preds = %64
  br label %73

72:                                               ; preds = %64
  br label %73

73:                                               ; preds = %72, %71, %70, %69, %68, %64
  %74 = phi float [ 1.000000e+00, %72 ], [ %12, %68 ], [ %13, %71 ], [ %14, %70 ], [ %11, %69 ], [ 0.000000e+00, %64 ]
  %75 = fmul contract float %67, %74
  %76 = fadd contract float %66, %75
  %77 = insertelement <4 x float> %57, float %76, i64 2
  %78 = lshr i32 %6, 16
  %79 = lshr i32 %6, 24
  %80 = extractelement <4 x float> %2, i64 3
  %81 = trunc i32 %78 to i8
  switch i8 %81, label %86 [
    i8 0, label %87
    i8 4, label %85
    i8 5, label %84
    i8 8, label %83
    i8 9, label %82
  ]

82:                                               ; preds = %73
  br label %87

83:                                               ; preds = %73
  br label %87

84:                                               ; preds = %73
  br label %87

85:                                               ; preds = %73
  br label %87

86:                                               ; preds = %73
  br label %87

87:                                               ; preds = %86, %85, %84, %83, %82, %73
  %88 = phi float [ 1.000000e+00, %86 ], [ %12, %82 ], [ %13, %85 ], [ %14, %84 ], [ %11, %83 ], [ 0.000000e+00, %73 ]
  %89 = fmul contract float %80, %88
  %90 = extractelement <4 x float> %10, i64 3
  %91 = trunc nuw i32 %79 to i8
  switch i8 %91, label %96 [
    i8 0, label %97
    i8 4, label %95
    i8 5, label %94
    i8 8, label %93
    i8 9, label %92
  ]

92:                                               ; preds = %87
  br label %97

93:                                               ; preds = %87
  br label %97

94:                                               ; preds = %87
  br label %97

95:                                               ; preds = %87
  br label %97

96:                                               ; preds = %87
  br label %97

97:                                               ; preds = %96, %95, %94, %93, %92, %87
  %98 = phi float [ 1.000000e+00, %96 ], [ %12, %92 ], [ %13, %95 ], [ %14, %94 ], [ %11, %93 ], [ 0.000000e+00, %87 ]
  %99 = fmul contract float %90, %98
  %100 = fadd contract float %89, %99
  %101 = insertelement <4 x float> %77, float %100, i64 3
  br label %102

102:                                              ; preds = %3, %97
  %103 = phi <4 x float> [ %101, %97 ], [ %2, %3 ]
  store <4 x float> %103, ptr %8, align 16, !tbaa !41
  ret void
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(write, argmem: readwrite, inaccessiblemem: none, target_mem: none) uwtable
define hidden void @rdna4_px_end(ptr nofree noundef readonly captures(none) %0, ptr nofree noundef readonly captures(none) %1) local_unnamed_addr #7 {
  %3 = getelementptr inbounds nuw i8, ptr %1, i64 32
  %4 = load i32, ptr %3, align 16, !tbaa !42
  %5 = icmp eq i32 %4, 0
  br i1 %5, label %65, label %6

6:                                                ; preds = %2
  %7 = getelementptr inbounds nuw i8, ptr %1, i64 16
  %8 = load <4 x float>, ptr %7, align 16, !tbaa !41
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
  %56 = load i32, ptr %55, align 4, !tbaa !40
  %57 = getelementptr inbounds nuw i8, ptr %0, i64 32
  %58 = load i32, ptr %57, align 8, !tbaa !20
  %59 = lshr i32 %58, 2
  %60 = mul i32 %59, %56
  %61 = load i32, ptr %1, align 16, !tbaa !38
  %62 = add i32 %60, %61
  %63 = zext i32 %62 to i64
  %64 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %54, i64 %63
  store i32 %52, ptr addrspace(1) %64, align 4, !tbaa !36
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
attributes #6 = { alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: readwrite) uwtable "amdgpu-no-cluster-id-x" "amdgpu-no-cluster-id-y" "amdgpu-no-cluster-id-z" "amdgpu-no-completion-action" "amdgpu-no-default-queue" "amdgpu-no-dispatch-id" "amdgpu-no-dispatch-ptr" "amdgpu-no-flat-scratch-init" "amdgpu-no-heap-ptr" "amdgpu-no-hostcall-ptr" "amdgpu-no-implicitarg-ptr" "amdgpu-no-lds-kernel-id" "amdgpu-no-multigrid-sync-arg" "amdgpu-no-queue-ptr" "amdgpu-no-workgroup-id-x" "amdgpu-no-workgroup-id-y" "amdgpu-no-workgroup-id-z" "amdgpu-no-workitem-id-x" "amdgpu-no-workitem-id-y" "amdgpu-no-workitem-id-z" "amdgpu-no-wwm" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="gfx1201" }
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
!10 = !{!"rdna4_draw", !11, i64 0, !6, i64 8, !6, i64 12, !6, i64 16, !6, i64 20, !13, i64 24, !6, i64 32, !6, i64 36, !6, i64 40, !6, i64 44, !14, i64 48, !14, i64 52, !14, i64 56, !14, i64 60, !6, i64 64}
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
!29 = !{!30, !6, i64 0}
!30 = !{!"rdna4_sampler", !6, i64 0}
!31 = !{!32, !6, i64 8}
!32 = !{!"rdna4_texture", !13, i64 0, !6, i64 8, !6, i64 12, !6, i64 16, !6, i64 20}
!33 = !{!32, !6, i64 12}
!34 = !{!32, !13, i64 0}
!35 = !{!32, !6, i64 16}
!36 = !{!6, !6, i64 0}
!37 = !{!32, !6, i64 20}
!38 = !{!39, !6, i64 0}
!39 = !{!"rdna4_px", !6, i64 0, !6, i64 4, !7, i64 16, !6, i64 32}
!40 = !{!39, !6, i64 4}
!41 = !{!7, !7, i64 0}
!42 = !{!39, !6, i64 32}
!43 = !{!11, !11, i64 0}
!44 = !{!14, !14, i64 0}
!45 = !{!46, !14, i64 48}
!46 = !{!"rdna4_tri", !7, i64 0, !7, i64 24, !7, i64 36, !14, i64 48, !14, i64 52, !6, i64 56}
!47 = !{!46, !14, i64 52}
!48 = !{!46, !6, i64 56}
!49 = distinct !{!49, !50}
!50 = !{!"llvm.loop.mustprogress"}

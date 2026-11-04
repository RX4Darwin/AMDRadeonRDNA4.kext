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
  br i1 %28, label %497, label %29

29:                                               ; preds = %24
  %30 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 12
  %31 = load i32, ptr addrspace(1) %30, align 4, !tbaa !37
  %32 = icmp eq i32 %31, 0
  br i1 %32, label %497, label %33

33:                                               ; preds = %29
  %34 = and i32 %4, 12
  %35 = icmp eq i32 %34, 0
  br i1 %35, label %36, label %149

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
  br i1 %104, label %105, label %497

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
  %117 = and i32 %116, 65535
  %118 = icmp eq i32 %117, 55
  br i1 %118, label %119, label %123

119:                                              ; preds = %105
  %120 = insertelement <4 x i32> poison, i32 %114, i64 0
  %121 = bitcast <4 x i32> %120 to <4 x float>
  %122 = shufflevector <4 x float> %121, <4 x float> <float poison, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 5, i32 6, i32 7>
  br label %497

123:                                              ; preds = %105
  %124 = and i32 %114, 255
  %125 = uitofp contract nneg i32 %124 to float
  %126 = fdiv contract float %125, 2.550000e+02
  %127 = insertelement <4 x float> poison, float %126, i64 0
  %128 = lshr i32 %114, 8
  %129 = and i32 %128, 255
  %130 = uitofp contract nneg i32 %129 to float
  %131 = fdiv contract float %130, 2.550000e+02
  %132 = insertelement <4 x float> %127, float %131, i64 1
  %133 = lshr i32 %114, 16
  %134 = and i32 %133, 255
  %135 = uitofp contract nneg i32 %134 to float
  %136 = fdiv contract float %135, 2.550000e+02
  %137 = insertelement <4 x float> %132, float %136, i64 2
  %138 = lshr i32 %114, 24
  %139 = uitofp contract nneg i32 %138 to float
  %140 = fdiv contract float %139, 2.550000e+02
  %141 = insertelement <4 x float> %137, float %140, i64 3
  %142 = and i32 %116, 65534
  %143 = icmp eq i32 %142, 80
  br i1 %143, label %144, label %497

144:                                              ; preds = %123
  %145 = insertelement <4 x float> poison, float %136, i64 0
  %146 = insertelement <4 x float> %145, float %131, i64 1
  %147 = insertelement <4 x float> %146, float %126, i64 2
  %148 = insertelement <4 x float> %147, float %140, i64 3
  br label %497

149:                                              ; preds = %33
  %150 = fadd contract float %17, -5.000000e-01
  %151 = fadd contract float %25, -5.000000e-01
  %152 = tail call contract float @llvm.floor.f32(float %150)
  %153 = fptosi float %152 to i32
  %154 = tail call contract float @llvm.floor.f32(float %151)
  %155 = fptosi float %154 to i32
  %156 = sitofp contract i32 %153 to float
  %157 = fsub contract float %150, %156
  %158 = sitofp contract i32 %155 to float
  %159 = fsub contract float %151, %158
  switch i32 %6, label %184 [
    i32 2, label %160
    i32 3, label %165
    i32 1, label %175
    i32 4, label %180
  ]

160:                                              ; preds = %149
  %161 = srem i32 %153, %27
  %162 = icmp slt i32 %161, 0
  %163 = select i1 %162, i32 %27, i32 0
  %164 = add nsw i32 %163, %161
  br label %189

165:                                              ; preds = %149
  %166 = shl nsw i32 %27, 1
  %167 = srem i32 %153, %166
  %168 = icmp slt i32 %167, 0
  %169 = select i1 %168, i32 %166, i32 0
  %170 = add nsw i32 %169, %167
  %171 = icmp slt i32 %170, %27
  %172 = xor i32 %170, -1
  %173 = add i32 %166, %172
  %174 = select i1 %171, i32 %170, i32 %173
  br label %189

175:                                              ; preds = %149
  %176 = ashr i32 %153, 31
  %177 = xor i32 %176, %153
  %178 = add nsw i32 %27, -1
  %179 = tail call i32 @llvm.smin.i32(i32 %177, i32 %178)
  br label %189

180:                                              ; preds = %149
  %181 = icmp sgt i32 %27, %153
  %182 = tail call i32 @llvm.smax.i32(i32 %153, i32 -1)
  %183 = select i1 %181, i32 %182, i32 -1
  br label %189

184:                                              ; preds = %149
  %185 = icmp slt i32 %153, 0
  %186 = add nsw i32 %27, -1
  %187 = tail call i32 @llvm.smin.i32(i32 %153, i32 %186)
  %188 = select i1 %185, i32 0, i32 %187
  br label %189

189:                                              ; preds = %160, %165, %175, %180, %184
  %190 = phi i32 [ %188, %184 ], [ %164, %160 ], [ %174, %165 ], [ %179, %175 ], [ %183, %180 ]
  %191 = add nsw i32 %153, 1
  switch i32 %6, label %216 [
    i32 2, label %192
    i32 3, label %197
    i32 1, label %207
    i32 4, label %212
  ]

192:                                              ; preds = %189
  %193 = srem i32 %191, %27
  %194 = icmp slt i32 %193, 0
  %195 = select i1 %194, i32 %27, i32 0
  %196 = add nsw i32 %195, %193
  br label %221

197:                                              ; preds = %189
  %198 = shl nsw i32 %27, 1
  %199 = srem i32 %191, %198
  %200 = icmp slt i32 %199, 0
  %201 = select i1 %200, i32 %198, i32 0
  %202 = add nsw i32 %201, %199
  %203 = icmp slt i32 %202, %27
  %204 = xor i32 %202, -1
  %205 = add i32 %198, %204
  %206 = select i1 %203, i32 %202, i32 %205
  br label %221

207:                                              ; preds = %189
  %208 = ashr i32 %191, 31
  %209 = xor i32 %208, %191
  %210 = add nsw i32 %27, -1
  %211 = tail call i32 @llvm.smin.i32(i32 %209, i32 %210)
  br label %221

212:                                              ; preds = %189
  %213 = icmp slt i32 %191, %27
  %214 = tail call i32 @llvm.smax.i32(i32 %191, i32 -1)
  %215 = select i1 %213, i32 %214, i32 -1
  br label %221

216:                                              ; preds = %189
  %217 = icmp slt i32 %153, -1
  %218 = add nsw i32 %27, -1
  %219 = tail call i32 @llvm.smin.i32(i32 %191, i32 %218)
  %220 = select i1 %217, i32 0, i32 %219
  br label %221

221:                                              ; preds = %192, %197, %207, %212, %216
  %222 = phi i32 [ %220, %216 ], [ %196, %192 ], [ %206, %197 ], [ %211, %207 ], [ %215, %212 ]
  switch i32 %8, label %247 [
    i32 2, label %223
    i32 3, label %228
    i32 1, label %238
    i32 4, label %243
  ]

223:                                              ; preds = %221
  %224 = srem i32 %155, %31
  %225 = icmp slt i32 %224, 0
  %226 = select i1 %225, i32 %31, i32 0
  %227 = add nsw i32 %226, %224
  br label %252

228:                                              ; preds = %221
  %229 = shl nsw i32 %31, 1
  %230 = srem i32 %155, %229
  %231 = icmp slt i32 %230, 0
  %232 = select i1 %231, i32 %229, i32 0
  %233 = add nsw i32 %232, %230
  %234 = icmp slt i32 %233, %31
  %235 = xor i32 %233, -1
  %236 = add i32 %229, %235
  %237 = select i1 %234, i32 %233, i32 %236
  br label %252

238:                                              ; preds = %221
  %239 = ashr i32 %155, 31
  %240 = xor i32 %239, %155
  %241 = add nsw i32 %31, -1
  %242 = tail call i32 @llvm.smin.i32(i32 %240, i32 %241)
  br label %252

243:                                              ; preds = %221
  %244 = icmp sgt i32 %31, %155
  %245 = tail call i32 @llvm.smax.i32(i32 %155, i32 -1)
  %246 = select i1 %244, i32 %245, i32 -1
  br label %252

247:                                              ; preds = %221
  %248 = icmp slt i32 %155, 0
  %249 = add nsw i32 %31, -1
  %250 = tail call i32 @llvm.smin.i32(i32 %155, i32 %249)
  %251 = select i1 %248, i32 0, i32 %250
  br label %252

252:                                              ; preds = %223, %228, %238, %243, %247
  %253 = phi i32 [ %251, %247 ], [ %227, %223 ], [ %237, %228 ], [ %242, %238 ], [ %246, %243 ]
  %254 = add nsw i32 %155, 1
  switch i32 %8, label %279 [
    i32 2, label %255
    i32 3, label %260
    i32 1, label %270
    i32 4, label %275
  ]

255:                                              ; preds = %252
  %256 = srem i32 %254, %31
  %257 = icmp slt i32 %256, 0
  %258 = select i1 %257, i32 %31, i32 0
  %259 = add nsw i32 %258, %256
  br label %284

260:                                              ; preds = %252
  %261 = shl nsw i32 %31, 1
  %262 = srem i32 %254, %261
  %263 = icmp slt i32 %262, 0
  %264 = select i1 %263, i32 %261, i32 0
  %265 = add nsw i32 %264, %262
  %266 = icmp slt i32 %265, %31
  %267 = xor i32 %265, -1
  %268 = add i32 %261, %267
  %269 = select i1 %266, i32 %265, i32 %268
  br label %284

270:                                              ; preds = %252
  %271 = ashr i32 %254, 31
  %272 = xor i32 %271, %254
  %273 = add nsw i32 %31, -1
  %274 = tail call i32 @llvm.smin.i32(i32 %272, i32 %273)
  br label %284

275:                                              ; preds = %252
  %276 = icmp slt i32 %254, %31
  %277 = tail call i32 @llvm.smax.i32(i32 %254, i32 -1)
  %278 = select i1 %276, i32 %277, i32 -1
  br label %284

279:                                              ; preds = %252
  %280 = icmp slt i32 %155, -1
  %281 = add nsw i32 %31, -1
  %282 = tail call i32 @llvm.smin.i32(i32 %254, i32 %281)
  %283 = select i1 %280, i32 0, i32 %282
  br label %284

284:                                              ; preds = %255, %260, %270, %275, %279
  %285 = phi i32 [ %283, %279 ], [ %259, %255 ], [ %269, %260 ], [ %274, %270 ], [ %278, %275 ]
  %286 = or i32 %253, %190
  %287 = icmp sgt i32 %286, -1
  br i1 %287, label %288, label %332

288:                                              ; preds = %284
  %289 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %290 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %291 = load i32, ptr addrspace(1) %290, align 8, !tbaa !39
  %292 = lshr i32 %291, 2
  %293 = mul i32 %292, %253
  %294 = add i32 %293, %190
  %295 = zext i32 %294 to i64
  %296 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %289, i64 %295
  %297 = load i32, ptr addrspace(1) %296, align 4, !tbaa !40
  %298 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %299 = load i32, ptr addrspace(1) %298, align 4, !tbaa !41
  %300 = and i32 %299, 65535
  %301 = icmp eq i32 %300, 55
  br i1 %301, label %302, label %306

302:                                              ; preds = %288
  %303 = insertelement <4 x i32> poison, i32 %297, i64 0
  %304 = bitcast <4 x i32> %303 to <4 x float>
  %305 = shufflevector <4 x float> %304, <4 x float> <float poison, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 5, i32 6, i32 7>
  br label %332

306:                                              ; preds = %288
  %307 = and i32 %297, 255
  %308 = uitofp contract nneg i32 %307 to float
  %309 = fdiv contract float %308, 2.550000e+02
  %310 = insertelement <4 x float> poison, float %309, i64 0
  %311 = lshr i32 %297, 8
  %312 = and i32 %311, 255
  %313 = uitofp contract nneg i32 %312 to float
  %314 = fdiv contract float %313, 2.550000e+02
  %315 = insertelement <4 x float> %310, float %314, i64 1
  %316 = lshr i32 %297, 16
  %317 = and i32 %316, 255
  %318 = uitofp contract nneg i32 %317 to float
  %319 = fdiv contract float %318, 2.550000e+02
  %320 = insertelement <4 x float> %315, float %319, i64 2
  %321 = lshr i32 %297, 24
  %322 = uitofp contract nneg i32 %321 to float
  %323 = fdiv contract float %322, 2.550000e+02
  %324 = insertelement <4 x float> %320, float %323, i64 3
  %325 = and i32 %299, 65534
  %326 = icmp eq i32 %325, 80
  br i1 %326, label %327, label %332

327:                                              ; preds = %306
  %328 = insertelement <4 x float> poison, float %319, i64 0
  %329 = insertelement <4 x float> %328, float %314, i64 1
  %330 = insertelement <4 x float> %329, float %309, i64 2
  %331 = insertelement <4 x float> %330, float %323, i64 3
  br label %332

332:                                              ; preds = %284, %302, %306, %327
  %333 = phi <4 x float> [ zeroinitializer, %284 ], [ %305, %302 ], [ %331, %327 ], [ %324, %306 ]
  %334 = fsub contract float 1.000000e+00, %157
  %335 = insertelement <4 x float> poison, float %334, i64 0
  %336 = shufflevector <4 x float> %335, <4 x float> poison, <4 x i32> zeroinitializer
  %337 = fmul contract <4 x float> %336, %333
  %338 = or i32 %253, %222
  %339 = icmp sgt i32 %338, -1
  br i1 %339, label %340, label %384

340:                                              ; preds = %332
  %341 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %342 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %343 = load i32, ptr addrspace(1) %342, align 8, !tbaa !39
  %344 = lshr i32 %343, 2
  %345 = mul i32 %344, %253
  %346 = add i32 %345, %222
  %347 = zext i32 %346 to i64
  %348 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %341, i64 %347
  %349 = load i32, ptr addrspace(1) %348, align 4, !tbaa !40
  %350 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %351 = load i32, ptr addrspace(1) %350, align 4, !tbaa !41
  %352 = and i32 %351, 65535
  %353 = icmp eq i32 %352, 55
  br i1 %353, label %354, label %358

354:                                              ; preds = %340
  %355 = insertelement <4 x i32> poison, i32 %349, i64 0
  %356 = bitcast <4 x i32> %355 to <4 x float>
  %357 = shufflevector <4 x float> %356, <4 x float> <float poison, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 5, i32 6, i32 7>
  br label %384

358:                                              ; preds = %340
  %359 = and i32 %349, 255
  %360 = uitofp contract nneg i32 %359 to float
  %361 = fdiv contract float %360, 2.550000e+02
  %362 = insertelement <4 x float> poison, float %361, i64 0
  %363 = lshr i32 %349, 8
  %364 = and i32 %363, 255
  %365 = uitofp contract nneg i32 %364 to float
  %366 = fdiv contract float %365, 2.550000e+02
  %367 = insertelement <4 x float> %362, float %366, i64 1
  %368 = lshr i32 %349, 16
  %369 = and i32 %368, 255
  %370 = uitofp contract nneg i32 %369 to float
  %371 = fdiv contract float %370, 2.550000e+02
  %372 = insertelement <4 x float> %367, float %371, i64 2
  %373 = lshr i32 %349, 24
  %374 = uitofp contract nneg i32 %373 to float
  %375 = fdiv contract float %374, 2.550000e+02
  %376 = insertelement <4 x float> %372, float %375, i64 3
  %377 = and i32 %351, 65534
  %378 = icmp eq i32 %377, 80
  br i1 %378, label %379, label %384

379:                                              ; preds = %358
  %380 = insertelement <4 x float> poison, float %371, i64 0
  %381 = insertelement <4 x float> %380, float %366, i64 1
  %382 = insertelement <4 x float> %381, float %361, i64 2
  %383 = insertelement <4 x float> %382, float %375, i64 3
  br label %384

384:                                              ; preds = %332, %354, %358, %379
  %385 = phi <4 x float> [ zeroinitializer, %332 ], [ %357, %354 ], [ %383, %379 ], [ %376, %358 ]
  %386 = insertelement <4 x float> poison, float %157, i64 0
  %387 = shufflevector <4 x float> %386, <4 x float> poison, <4 x i32> zeroinitializer
  %388 = fmul contract <4 x float> %387, %385
  %389 = fadd contract <4 x float> %337, %388
  %390 = or i32 %285, %190
  %391 = icmp sgt i32 %390, -1
  br i1 %391, label %392, label %436

392:                                              ; preds = %384
  %393 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %394 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %395 = load i32, ptr addrspace(1) %394, align 8, !tbaa !39
  %396 = lshr i32 %395, 2
  %397 = mul i32 %396, %285
  %398 = add i32 %397, %190
  %399 = zext i32 %398 to i64
  %400 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %393, i64 %399
  %401 = load i32, ptr addrspace(1) %400, align 4, !tbaa !40
  %402 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %403 = load i32, ptr addrspace(1) %402, align 4, !tbaa !41
  %404 = and i32 %403, 65535
  %405 = icmp eq i32 %404, 55
  br i1 %405, label %406, label %410

406:                                              ; preds = %392
  %407 = insertelement <4 x i32> poison, i32 %401, i64 0
  %408 = bitcast <4 x i32> %407 to <4 x float>
  %409 = shufflevector <4 x float> %408, <4 x float> <float poison, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 5, i32 6, i32 7>
  br label %436

410:                                              ; preds = %392
  %411 = and i32 %401, 255
  %412 = uitofp contract nneg i32 %411 to float
  %413 = fdiv contract float %412, 2.550000e+02
  %414 = insertelement <4 x float> poison, float %413, i64 0
  %415 = lshr i32 %401, 8
  %416 = and i32 %415, 255
  %417 = uitofp contract nneg i32 %416 to float
  %418 = fdiv contract float %417, 2.550000e+02
  %419 = insertelement <4 x float> %414, float %418, i64 1
  %420 = lshr i32 %401, 16
  %421 = and i32 %420, 255
  %422 = uitofp contract nneg i32 %421 to float
  %423 = fdiv contract float %422, 2.550000e+02
  %424 = insertelement <4 x float> %419, float %423, i64 2
  %425 = lshr i32 %401, 24
  %426 = uitofp contract nneg i32 %425 to float
  %427 = fdiv contract float %426, 2.550000e+02
  %428 = insertelement <4 x float> %424, float %427, i64 3
  %429 = and i32 %403, 65534
  %430 = icmp eq i32 %429, 80
  br i1 %430, label %431, label %436

431:                                              ; preds = %410
  %432 = insertelement <4 x float> poison, float %423, i64 0
  %433 = insertelement <4 x float> %432, float %418, i64 1
  %434 = insertelement <4 x float> %433, float %413, i64 2
  %435 = insertelement <4 x float> %434, float %427, i64 3
  br label %436

436:                                              ; preds = %384, %406, %410, %431
  %437 = phi <4 x float> [ zeroinitializer, %384 ], [ %409, %406 ], [ %435, %431 ], [ %428, %410 ]
  %438 = fmul contract <4 x float> %336, %437
  %439 = or i32 %285, %222
  %440 = icmp sgt i32 %439, -1
  br i1 %440, label %441, label %485

441:                                              ; preds = %436
  %442 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %443 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %444 = load i32, ptr addrspace(1) %443, align 8, !tbaa !39
  %445 = lshr i32 %444, 2
  %446 = mul i32 %445, %285
  %447 = add i32 %446, %222
  %448 = zext i32 %447 to i64
  %449 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %442, i64 %448
  %450 = load i32, ptr addrspace(1) %449, align 4, !tbaa !40
  %451 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %452 = load i32, ptr addrspace(1) %451, align 4, !tbaa !41
  %453 = and i32 %452, 65535
  %454 = icmp eq i32 %453, 55
  br i1 %454, label %455, label %459

455:                                              ; preds = %441
  %456 = insertelement <4 x i32> poison, i32 %450, i64 0
  %457 = bitcast <4 x i32> %456 to <4 x float>
  %458 = shufflevector <4 x float> %457, <4 x float> <float poison, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 5, i32 6, i32 7>
  br label %485

459:                                              ; preds = %441
  %460 = and i32 %450, 255
  %461 = uitofp contract nneg i32 %460 to float
  %462 = fdiv contract float %461, 2.550000e+02
  %463 = insertelement <4 x float> poison, float %462, i64 0
  %464 = lshr i32 %450, 8
  %465 = and i32 %464, 255
  %466 = uitofp contract nneg i32 %465 to float
  %467 = fdiv contract float %466, 2.550000e+02
  %468 = insertelement <4 x float> %463, float %467, i64 1
  %469 = lshr i32 %450, 16
  %470 = and i32 %469, 255
  %471 = uitofp contract nneg i32 %470 to float
  %472 = fdiv contract float %471, 2.550000e+02
  %473 = insertelement <4 x float> %468, float %472, i64 2
  %474 = lshr i32 %450, 24
  %475 = uitofp contract nneg i32 %474 to float
  %476 = fdiv contract float %475, 2.550000e+02
  %477 = insertelement <4 x float> %473, float %476, i64 3
  %478 = and i32 %452, 65534
  %479 = icmp eq i32 %478, 80
  br i1 %479, label %480, label %485

480:                                              ; preds = %459
  %481 = insertelement <4 x float> poison, float %472, i64 0
  %482 = insertelement <4 x float> %481, float %467, i64 1
  %483 = insertelement <4 x float> %482, float %462, i64 2
  %484 = insertelement <4 x float> %483, float %476, i64 3
  br label %485

485:                                              ; preds = %436, %455, %459, %480
  %486 = phi <4 x float> [ zeroinitializer, %436 ], [ %458, %455 ], [ %484, %480 ], [ %477, %459 ]
  %487 = fmul contract <4 x float> %387, %486
  %488 = fadd contract <4 x float> %438, %487
  %489 = fsub contract float 1.000000e+00, %159
  %490 = insertelement <4 x float> poison, float %489, i64 0
  %491 = shufflevector <4 x float> %490, <4 x float> poison, <4 x i32> zeroinitializer
  %492 = fmul contract <4 x float> %491, %389
  %493 = insertelement <4 x float> poison, float %159, i64 0
  %494 = shufflevector <4 x float> %493, <4 x float> poison, <4 x i32> zeroinitializer
  %495 = fmul contract <4 x float> %494, %488
  %496 = fadd contract <4 x float> %492, %495
  br label %497

497:                                              ; preds = %144, %123, %119, %101, %24, %29, %485
  %498 = phi <4 x float> [ zeroinitializer, %24 ], [ %496, %485 ], [ zeroinitializer, %29 ], [ zeroinitializer, %101 ], [ %122, %119 ], [ %148, %144 ], [ %141, %123 ]
  ret <4 x float> %498
}

; Function Attrs: mustprogress nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare float @llvm.floor.f32(float) #2

; Function Attrs: alwaysinline nofree norecurse nosync nounwind memory(read, inaccessiblemem: none, target_mem: none) uwtable
define hidden <4 x float> @rdna4_sample_3d(ptr addrspace(1) nofree noundef readonly captures(none) %0, ptr addrspace(4) nofree noundef readonly captures(none) %1, <3 x float> noundef %2) local_unnamed_addr #3 {
  %4 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %5 = load i32, ptr addrspace(1) %4, align 4, !tbaa !41
  %6 = lshr i32 %5, 16
  %7 = tail call i32 @llvm.umax.i32(i32 %6, i32 1)
  %8 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 12
  %9 = load i32, ptr addrspace(1) %8, align 4, !tbaa !37
  %10 = udiv i32 %9, %7
  %11 = load i32, ptr addrspace(4) %1, align 4, !tbaa !33
  %12 = lshr i32 %11, 5
  %13 = and i32 %12, 7
  %14 = lshr i32 %11, 8
  %15 = and i32 %14, 7
  %16 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 8
  %17 = load i32, ptr addrspace(1) %16, align 8, !tbaa !35
  %18 = icmp ne i32 %17, 0
  %19 = icmp ule i32 %7, %9
  %20 = select i1 %18, i1 %19, i1 false
  br i1 %20, label %21, label %501

21:                                               ; preds = %3
  %22 = icmp sgt i32 %11, -1
  %23 = extractelement <3 x float> %2, i64 0
  %24 = uitofp contract i32 %17 to float
  %25 = fmul contract float %23, %24
  %26 = select contract i1 %22, float %23, float %25
  %27 = extractelement <3 x float> %2, i64 1
  %28 = uitofp contract i32 %10 to float
  %29 = fmul contract float %27, %28
  %30 = select contract i1 %22, float %27, float %29
  %31 = extractelement <3 x float> %2, i64 2
  %32 = uitofp contract nneg i32 %7 to float
  %33 = fmul contract float %31, %32
  %34 = select contract i1 %22, float %31, float %33
  %35 = and i32 %11, 12
  %36 = icmp eq i32 %35, 0
  %37 = tail call contract float @llvm.floor.f32(float %34)
  %38 = fadd contract float %34, -5.000000e-01
  %39 = tail call contract float @llvm.floor.f32(float %38)
  %40 = select i1 %36, float %37, float %39
  %41 = fptosi float %40 to i32
  %42 = icmp slt i32 %41, 0
  %43 = add nsw i32 %7, -1
  %44 = tail call i32 @llvm.smin.i32(i32 %41, i32 %43)
  %45 = select i1 %42, i32 0, i32 %44
  %46 = icmp slt i32 %41, -1
  %47 = add nsw i32 %41, 1
  %48 = tail call i32 @llvm.smin.i32(i32 %47, i32 %43)
  %49 = select i1 %46, i32 0, i32 %48
  %50 = fadd contract float %26, -5.000000e-01
  %51 = fadd contract float %30, -5.000000e-01
  %52 = tail call contract float @llvm.floor.f32(float %50)
  %53 = fptosi float %52 to i32
  %54 = tail call contract float @llvm.floor.f32(float %51)
  %55 = fptosi float %54 to i32
  %56 = sitofp contract i32 %53 to float
  %57 = fsub contract float %50, %56
  %58 = sitofp contract i32 %55 to float
  %59 = fsub contract float %51, %58
  %60 = icmp sgt i32 %17, %53
  %61 = tail call i32 @llvm.smax.i32(i32 %53, i32 -1)
  %62 = select i1 %60, i32 %61, i32 -1
  %63 = ashr i32 %53, 31
  %64 = xor i32 %63, %53
  %65 = add nsw i32 %17, -1
  %66 = tail call i32 @llvm.smin.i32(i32 %64, i32 %65)
  %67 = shl nsw i32 %17, 1
  %68 = icmp slt i32 %53, 0
  %69 = tail call i32 @llvm.smin.i32(i32 %53, i32 %65)
  %70 = select i1 %68, i32 0, i32 %69
  %71 = add nsw i32 %53, 1
  %72 = icmp slt i32 %71, %17
  %73 = tail call i32 @llvm.smax.i32(i32 %71, i32 -1)
  %74 = select i1 %72, i32 %73, i32 -1
  %75 = ashr i32 %71, 31
  %76 = xor i32 %75, %71
  %77 = tail call i32 @llvm.smin.i32(i32 %76, i32 %65)
  %78 = icmp slt i32 %53, -1
  %79 = tail call i32 @llvm.smin.i32(i32 %71, i32 %65)
  %80 = select i1 %78, i32 0, i32 %79
  %81 = icmp sgt i32 %10, %55
  %82 = tail call i32 @llvm.smax.i32(i32 %55, i32 -1)
  %83 = select i1 %81, i32 %82, i32 -1
  %84 = ashr i32 %55, 31
  %85 = xor i32 %84, %55
  %86 = add nsw i32 %10, -1
  %87 = tail call i32 @llvm.smin.i32(i32 %85, i32 %86)
  %88 = shl nsw i32 %10, 1
  %89 = icmp slt i32 %55, 0
  %90 = tail call i32 @llvm.smin.i32(i32 %55, i32 %86)
  %91 = select i1 %89, i32 0, i32 %90
  %92 = add nsw i32 %55, 1
  %93 = icmp slt i32 %92, %10
  %94 = tail call i32 @llvm.smax.i32(i32 %92, i32 -1)
  %95 = select i1 %93, i32 %94, i32 -1
  %96 = ashr i32 %92, 31
  %97 = xor i32 %96, %92
  %98 = tail call i32 @llvm.smin.i32(i32 %97, i32 %86)
  %99 = icmp slt i32 %55, -1
  %100 = tail call i32 @llvm.smin.i32(i32 %92, i32 %86)
  %101 = select i1 %99, i32 0, i32 %100
  %102 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %103 = and i32 %5, 65535
  %104 = icmp eq i32 %103, 55
  %105 = and i32 %5, 65534
  %106 = icmp eq i32 %105, 80
  %107 = fsub contract float 1.000000e+00, %57
  %108 = insertelement <4 x float> poison, float %107, i64 0
  %109 = shufflevector <4 x float> %108, <4 x float> poison, <4 x i32> zeroinitializer
  %110 = insertelement <4 x float> poison, float %57, i64 0
  %111 = shufflevector <4 x float> %110, <4 x float> poison, <4 x i32> zeroinitializer
  %112 = fsub contract float 1.000000e+00, %59
  %113 = insertelement <4 x float> poison, float %112, i64 0
  %114 = shufflevector <4 x float> %113, <4 x float> poison, <4 x i32> zeroinitializer
  %115 = insertelement <4 x float> poison, float %59, i64 0
  %116 = shufflevector <4 x float> %115, <4 x float> poison, <4 x i32> zeroinitializer
  %117 = tail call contract float @llvm.floor.f32(float %26)
  %118 = fptosi float %117 to i32
  %119 = icmp sgt i32 %17, %118
  %120 = tail call i32 @llvm.smax.i32(i32 %118, i32 -1)
  %121 = select i1 %119, i32 %120, i32 -1
  %122 = ashr i32 %118, 31
  %123 = xor i32 %122, %118
  %124 = tail call i32 @llvm.smin.i32(i32 %123, i32 %65)
  %125 = icmp slt i32 %118, 0
  %126 = tail call i32 @llvm.smin.i32(i32 %118, i32 %65)
  %127 = select i1 %125, i32 0, i32 %126
  %128 = tail call contract float @llvm.floor.f32(float %30)
  %129 = fptosi float %128 to i32
  %130 = icmp sgt i32 %10, %129
  %131 = tail call i32 @llvm.smax.i32(i32 %129, i32 -1)
  %132 = select i1 %130, i32 %131, i32 -1
  %133 = ashr i32 %129, 31
  %134 = xor i32 %133, %129
  %135 = tail call i32 @llvm.smin.i32(i32 %134, i32 %86)
  %136 = icmp slt i32 %129, 0
  %137 = tail call i32 @llvm.smin.i32(i32 %129, i32 %86)
  %138 = select i1 %136, i32 0, i32 %137
  br label %153

139:                                              ; preds = %488
  %140 = sitofp contract i32 %41 to float
  %141 = fsub contract float %38, %140
  %142 = select contract i1 %36, float 0.000000e+00, float %141
  %143 = shufflevector <8 x float> %500, <8 x float> poison, <4 x i32> <i32 0, i32 1, i32 2, i32 3>
  %144 = fsub contract float 1.000000e+00, %142
  %145 = insertelement <4 x float> poison, float %144, i64 0
  %146 = shufflevector <4 x float> %145, <4 x float> poison, <4 x i32> zeroinitializer
  %147 = fmul contract <4 x float> %146, %143
  %148 = shufflevector <8 x float> %500, <8 x float> poison, <4 x i32> <i32 4, i32 5, i32 6, i32 7>
  %149 = insertelement <4 x float> poison, float %142, i64 0
  %150 = shufflevector <4 x float> %149, <4 x float> poison, <4 x i32> zeroinitializer
  %151 = fmul contract <4 x float> %150, %148
  %152 = fadd contract <4 x float> %147, %151
  br label %501

153:                                              ; preds = %21, %488
  %154 = phi i1 [ true, %21 ], [ false, %488 ]
  %155 = phi i32 [ 0, %21 ], [ 4, %488 ]
  %156 = phi <8 x float> [ zeroinitializer, %21 ], [ %500, %488 ]
  %157 = select i1 %154, i32 %45, i32 %49
  %158 = mul nsw i32 %157, %10
  br i1 %36, label %159, label %236

159:                                              ; preds = %153
  switch i32 %13, label %175 [
    i32 2, label %160
    i32 3, label %165
    i32 1, label %176
    i32 4, label %174
  ]

160:                                              ; preds = %159
  %161 = srem i32 %118, %17
  %162 = icmp slt i32 %161, 0
  %163 = select i1 %162, i32 %17, i32 0
  %164 = add nsw i32 %163, %161
  br label %176

165:                                              ; preds = %159
  %166 = srem i32 %118, %67
  %167 = icmp slt i32 %166, 0
  %168 = select i1 %167, i32 %67, i32 0
  %169 = add nsw i32 %168, %166
  %170 = icmp slt i32 %169, %17
  %171 = xor i32 %169, -1
  %172 = add i32 %67, %171
  %173 = select i1 %170, i32 %169, i32 %172
  br label %176

174:                                              ; preds = %159
  br label %176

175:                                              ; preds = %159
  br label %176

176:                                              ; preds = %159, %160, %165, %174, %175
  %177 = phi i32 [ %127, %175 ], [ %164, %160 ], [ %173, %165 ], [ %121, %174 ], [ %124, %159 ]
  switch i32 %15, label %193 [
    i32 2, label %178
    i32 3, label %183
    i32 1, label %194
    i32 4, label %192
  ]

178:                                              ; preds = %176
  %179 = srem i32 %129, %10
  %180 = icmp slt i32 %179, 0
  %181 = select i1 %180, i32 %10, i32 0
  %182 = add nsw i32 %181, %179
  br label %194

183:                                              ; preds = %176
  %184 = srem i32 %129, %88
  %185 = icmp slt i32 %184, 0
  %186 = select i1 %185, i32 %88, i32 0
  %187 = add nsw i32 %186, %184
  %188 = icmp slt i32 %187, %10
  %189 = xor i32 %187, -1
  %190 = add i32 %88, %189
  %191 = select i1 %188, i32 %187, i32 %190
  br label %194

192:                                              ; preds = %176
  br label %194

193:                                              ; preds = %176
  br label %194

194:                                              ; preds = %176, %178, %183, %192, %193
  %195 = phi i32 [ %138, %193 ], [ %182, %178 ], [ %191, %183 ], [ %132, %192 ], [ %135, %176 ]
  %196 = add nsw i32 %195, %158
  %197 = or i32 %196, %177
  %198 = icmp sgt i32 %197, -1
  br i1 %198, label %199, label %488

199:                                              ; preds = %194
  %200 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %201 = load i32, ptr addrspace(1) %102, align 8, !tbaa !39
  %202 = lshr i32 %201, 2
  %203 = mul i32 %202, %196
  %204 = add i32 %203, %177
  %205 = zext i32 %204 to i64
  %206 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %200, i64 %205
  %207 = load i32, ptr addrspace(1) %206, align 4, !tbaa !40
  br i1 %104, label %208, label %212

208:                                              ; preds = %199
  %209 = insertelement <4 x i32> poison, i32 %207, i64 0
  %210 = bitcast <4 x i32> %209 to <4 x float>
  %211 = shufflevector <4 x float> %210, <4 x float> <float poison, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 5, i32 6, i32 7>
  br label %488

212:                                              ; preds = %199
  %213 = and i32 %207, 255
  %214 = uitofp contract nneg i32 %213 to float
  %215 = fdiv contract float %214, 2.550000e+02
  %216 = insertelement <4 x float> poison, float %215, i64 0
  %217 = lshr i32 %207, 8
  %218 = and i32 %217, 255
  %219 = uitofp contract nneg i32 %218 to float
  %220 = fdiv contract float %219, 2.550000e+02
  %221 = insertelement <4 x float> %216, float %220, i64 1
  %222 = lshr i32 %207, 16
  %223 = and i32 %222, 255
  %224 = uitofp contract nneg i32 %223 to float
  %225 = fdiv contract float %224, 2.550000e+02
  %226 = insertelement <4 x float> %221, float %225, i64 2
  %227 = lshr i32 %207, 24
  %228 = uitofp contract nneg i32 %227 to float
  %229 = fdiv contract float %228, 2.550000e+02
  %230 = insertelement <4 x float> %226, float %229, i64 3
  br i1 %106, label %231, label %488

231:                                              ; preds = %212
  %232 = insertelement <4 x float> poison, float %225, i64 0
  %233 = insertelement <4 x float> %232, float %220, i64 1
  %234 = insertelement <4 x float> %233, float %215, i64 2
  %235 = insertelement <4 x float> %234, float %229, i64 3
  br label %488

236:                                              ; preds = %153
  switch i32 %13, label %252 [
    i32 2, label %237
    i32 3, label %242
    i32 1, label %253
    i32 4, label %251
  ]

237:                                              ; preds = %236
  %238 = srem i32 %53, %17
  %239 = icmp slt i32 %238, 0
  %240 = select i1 %239, i32 %17, i32 0
  %241 = add nsw i32 %240, %238
  br label %253

242:                                              ; preds = %236
  %243 = srem i32 %53, %67
  %244 = icmp slt i32 %243, 0
  %245 = select i1 %244, i32 %67, i32 0
  %246 = add nsw i32 %245, %243
  %247 = icmp slt i32 %246, %17
  %248 = xor i32 %246, -1
  %249 = add i32 %67, %248
  %250 = select i1 %247, i32 %246, i32 %249
  br label %253

251:                                              ; preds = %236
  br label %253

252:                                              ; preds = %236
  br label %253

253:                                              ; preds = %236, %237, %242, %251, %252
  %254 = phi i32 [ %70, %252 ], [ %241, %237 ], [ %250, %242 ], [ %62, %251 ], [ %66, %236 ]
  switch i32 %13, label %270 [
    i32 2, label %255
    i32 3, label %260
    i32 1, label %271
    i32 4, label %269
  ]

255:                                              ; preds = %253
  %256 = srem i32 %71, %17
  %257 = icmp slt i32 %256, 0
  %258 = select i1 %257, i32 %17, i32 0
  %259 = add nsw i32 %258, %256
  br label %271

260:                                              ; preds = %253
  %261 = srem i32 %71, %67
  %262 = icmp slt i32 %261, 0
  %263 = select i1 %262, i32 %67, i32 0
  %264 = add nsw i32 %263, %261
  %265 = icmp slt i32 %264, %17
  %266 = xor i32 %264, -1
  %267 = add i32 %67, %266
  %268 = select i1 %265, i32 %264, i32 %267
  br label %271

269:                                              ; preds = %253
  br label %271

270:                                              ; preds = %253
  br label %271

271:                                              ; preds = %253, %255, %260, %269, %270
  %272 = phi i32 [ %80, %270 ], [ %259, %255 ], [ %268, %260 ], [ %74, %269 ], [ %77, %253 ]
  switch i32 %15, label %288 [
    i32 2, label %273
    i32 3, label %278
    i32 1, label %289
    i32 4, label %287
  ]

273:                                              ; preds = %271
  %274 = srem i32 %55, %10
  %275 = icmp slt i32 %274, 0
  %276 = select i1 %275, i32 %10, i32 0
  %277 = add nsw i32 %276, %274
  br label %289

278:                                              ; preds = %271
  %279 = srem i32 %55, %88
  %280 = icmp slt i32 %279, 0
  %281 = select i1 %280, i32 %88, i32 0
  %282 = add nsw i32 %281, %279
  %283 = icmp slt i32 %282, %10
  %284 = xor i32 %282, -1
  %285 = add i32 %88, %284
  %286 = select i1 %283, i32 %282, i32 %285
  br label %289

287:                                              ; preds = %271
  br label %289

288:                                              ; preds = %271
  br label %289

289:                                              ; preds = %271, %273, %278, %287, %288
  %290 = phi i32 [ %91, %288 ], [ %277, %273 ], [ %286, %278 ], [ %83, %287 ], [ %87, %271 ]
  switch i32 %15, label %306 [
    i32 2, label %291
    i32 3, label %296
    i32 1, label %307
    i32 4, label %305
  ]

291:                                              ; preds = %289
  %292 = srem i32 %92, %10
  %293 = icmp slt i32 %292, 0
  %294 = select i1 %293, i32 %10, i32 0
  %295 = add nsw i32 %294, %292
  br label %307

296:                                              ; preds = %289
  %297 = srem i32 %92, %88
  %298 = icmp slt i32 %297, 0
  %299 = select i1 %298, i32 %88, i32 0
  %300 = add nsw i32 %299, %297
  %301 = icmp slt i32 %300, %10
  %302 = xor i32 %300, -1
  %303 = add i32 %88, %302
  %304 = select i1 %301, i32 %300, i32 %303
  br label %307

305:                                              ; preds = %289
  br label %307

306:                                              ; preds = %289
  br label %307

307:                                              ; preds = %289, %291, %296, %305, %306
  %308 = phi i32 [ %101, %306 ], [ %295, %291 ], [ %304, %296 ], [ %95, %305 ], [ %98, %289 ]
  %309 = add nsw i32 %290, %158
  %310 = icmp sgt i32 %290, -1
  %311 = select i1 %310, i32 %309, i32 -1
  %312 = or i32 %311, %254
  %313 = icmp sgt i32 %312, -1
  br i1 %313, label %314, label %351

314:                                              ; preds = %307
  %315 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %316 = load i32, ptr addrspace(1) %102, align 8, !tbaa !39
  %317 = lshr i32 %316, 2
  %318 = mul i32 %317, %311
  %319 = add i32 %318, %254
  %320 = zext i32 %319 to i64
  %321 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %315, i64 %320
  %322 = load i32, ptr addrspace(1) %321, align 4, !tbaa !40
  br i1 %104, label %323, label %327

323:                                              ; preds = %314
  %324 = insertelement <4 x i32> poison, i32 %322, i64 0
  %325 = bitcast <4 x i32> %324 to <4 x float>
  %326 = shufflevector <4 x float> %325, <4 x float> <float poison, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 5, i32 6, i32 7>
  br label %351

327:                                              ; preds = %314
  %328 = and i32 %322, 255
  %329 = uitofp contract nneg i32 %328 to float
  %330 = fdiv contract float %329, 2.550000e+02
  %331 = insertelement <4 x float> poison, float %330, i64 0
  %332 = lshr i32 %322, 8
  %333 = and i32 %332, 255
  %334 = uitofp contract nneg i32 %333 to float
  %335 = fdiv contract float %334, 2.550000e+02
  %336 = insertelement <4 x float> %331, float %335, i64 1
  %337 = lshr i32 %322, 16
  %338 = and i32 %337, 255
  %339 = uitofp contract nneg i32 %338 to float
  %340 = fdiv contract float %339, 2.550000e+02
  %341 = insertelement <4 x float> %336, float %340, i64 2
  %342 = lshr i32 %322, 24
  %343 = uitofp contract nneg i32 %342 to float
  %344 = fdiv contract float %343, 2.550000e+02
  %345 = insertelement <4 x float> %341, float %344, i64 3
  br i1 %106, label %346, label %351

346:                                              ; preds = %327
  %347 = insertelement <4 x float> poison, float %340, i64 0
  %348 = insertelement <4 x float> %347, float %335, i64 1
  %349 = insertelement <4 x float> %348, float %330, i64 2
  %350 = insertelement <4 x float> %349, float %344, i64 3
  br label %351

351:                                              ; preds = %307, %323, %327, %346
  %352 = phi <4 x float> [ zeroinitializer, %307 ], [ %326, %323 ], [ %350, %346 ], [ %345, %327 ]
  %353 = fmul contract <4 x float> %109, %352
  %354 = or i32 %311, %272
  %355 = icmp sgt i32 %354, -1
  br i1 %355, label %356, label %393

356:                                              ; preds = %351
  %357 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %358 = load i32, ptr addrspace(1) %102, align 8, !tbaa !39
  %359 = lshr i32 %358, 2
  %360 = mul i32 %359, %311
  %361 = add i32 %360, %272
  %362 = zext i32 %361 to i64
  %363 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %357, i64 %362
  %364 = load i32, ptr addrspace(1) %363, align 4, !tbaa !40
  br i1 %104, label %365, label %369

365:                                              ; preds = %356
  %366 = insertelement <4 x i32> poison, i32 %364, i64 0
  %367 = bitcast <4 x i32> %366 to <4 x float>
  %368 = shufflevector <4 x float> %367, <4 x float> <float poison, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 5, i32 6, i32 7>
  br label %393

369:                                              ; preds = %356
  %370 = and i32 %364, 255
  %371 = uitofp contract nneg i32 %370 to float
  %372 = fdiv contract float %371, 2.550000e+02
  %373 = insertelement <4 x float> poison, float %372, i64 0
  %374 = lshr i32 %364, 8
  %375 = and i32 %374, 255
  %376 = uitofp contract nneg i32 %375 to float
  %377 = fdiv contract float %376, 2.550000e+02
  %378 = insertelement <4 x float> %373, float %377, i64 1
  %379 = lshr i32 %364, 16
  %380 = and i32 %379, 255
  %381 = uitofp contract nneg i32 %380 to float
  %382 = fdiv contract float %381, 2.550000e+02
  %383 = insertelement <4 x float> %378, float %382, i64 2
  %384 = lshr i32 %364, 24
  %385 = uitofp contract nneg i32 %384 to float
  %386 = fdiv contract float %385, 2.550000e+02
  %387 = insertelement <4 x float> %383, float %386, i64 3
  br i1 %106, label %388, label %393

388:                                              ; preds = %369
  %389 = insertelement <4 x float> poison, float %382, i64 0
  %390 = insertelement <4 x float> %389, float %377, i64 1
  %391 = insertelement <4 x float> %390, float %372, i64 2
  %392 = insertelement <4 x float> %391, float %386, i64 3
  br label %393

393:                                              ; preds = %351, %365, %369, %388
  %394 = phi <4 x float> [ zeroinitializer, %351 ], [ %368, %365 ], [ %392, %388 ], [ %387, %369 ]
  %395 = fmul contract <4 x float> %111, %394
  %396 = fadd contract <4 x float> %353, %395
  %397 = add nsw i32 %308, %158
  %398 = icmp sgt i32 %308, -1
  %399 = select i1 %398, i32 %397, i32 -1
  %400 = or i32 %399, %254
  %401 = icmp sgt i32 %400, -1
  br i1 %401, label %402, label %439

402:                                              ; preds = %393
  %403 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %404 = load i32, ptr addrspace(1) %102, align 8, !tbaa !39
  %405 = lshr i32 %404, 2
  %406 = mul i32 %405, %399
  %407 = add i32 %406, %254
  %408 = zext i32 %407 to i64
  %409 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %403, i64 %408
  %410 = load i32, ptr addrspace(1) %409, align 4, !tbaa !40
  br i1 %104, label %411, label %415

411:                                              ; preds = %402
  %412 = insertelement <4 x i32> poison, i32 %410, i64 0
  %413 = bitcast <4 x i32> %412 to <4 x float>
  %414 = shufflevector <4 x float> %413, <4 x float> <float poison, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 5, i32 6, i32 7>
  br label %439

415:                                              ; preds = %402
  %416 = and i32 %410, 255
  %417 = uitofp contract nneg i32 %416 to float
  %418 = fdiv contract float %417, 2.550000e+02
  %419 = insertelement <4 x float> poison, float %418, i64 0
  %420 = lshr i32 %410, 8
  %421 = and i32 %420, 255
  %422 = uitofp contract nneg i32 %421 to float
  %423 = fdiv contract float %422, 2.550000e+02
  %424 = insertelement <4 x float> %419, float %423, i64 1
  %425 = lshr i32 %410, 16
  %426 = and i32 %425, 255
  %427 = uitofp contract nneg i32 %426 to float
  %428 = fdiv contract float %427, 2.550000e+02
  %429 = insertelement <4 x float> %424, float %428, i64 2
  %430 = lshr i32 %410, 24
  %431 = uitofp contract nneg i32 %430 to float
  %432 = fdiv contract float %431, 2.550000e+02
  %433 = insertelement <4 x float> %429, float %432, i64 3
  br i1 %106, label %434, label %439

434:                                              ; preds = %415
  %435 = insertelement <4 x float> poison, float %428, i64 0
  %436 = insertelement <4 x float> %435, float %423, i64 1
  %437 = insertelement <4 x float> %436, float %418, i64 2
  %438 = insertelement <4 x float> %437, float %432, i64 3
  br label %439

439:                                              ; preds = %393, %411, %415, %434
  %440 = phi <4 x float> [ zeroinitializer, %393 ], [ %414, %411 ], [ %438, %434 ], [ %433, %415 ]
  %441 = fmul contract <4 x float> %109, %440
  %442 = or i32 %399, %272
  %443 = icmp sgt i32 %442, -1
  br i1 %443, label %444, label %481

444:                                              ; preds = %439
  %445 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %446 = load i32, ptr addrspace(1) %102, align 8, !tbaa !39
  %447 = lshr i32 %446, 2
  %448 = mul i32 %447, %399
  %449 = add i32 %448, %272
  %450 = zext i32 %449 to i64
  %451 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %445, i64 %450
  %452 = load i32, ptr addrspace(1) %451, align 4, !tbaa !40
  br i1 %104, label %453, label %457

453:                                              ; preds = %444
  %454 = insertelement <4 x i32> poison, i32 %452, i64 0
  %455 = bitcast <4 x i32> %454 to <4 x float>
  %456 = shufflevector <4 x float> %455, <4 x float> <float poison, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 5, i32 6, i32 7>
  br label %481

457:                                              ; preds = %444
  %458 = and i32 %452, 255
  %459 = uitofp contract nneg i32 %458 to float
  %460 = fdiv contract float %459, 2.550000e+02
  %461 = insertelement <4 x float> poison, float %460, i64 0
  %462 = lshr i32 %452, 8
  %463 = and i32 %462, 255
  %464 = uitofp contract nneg i32 %463 to float
  %465 = fdiv contract float %464, 2.550000e+02
  %466 = insertelement <4 x float> %461, float %465, i64 1
  %467 = lshr i32 %452, 16
  %468 = and i32 %467, 255
  %469 = uitofp contract nneg i32 %468 to float
  %470 = fdiv contract float %469, 2.550000e+02
  %471 = insertelement <4 x float> %466, float %470, i64 2
  %472 = lshr i32 %452, 24
  %473 = uitofp contract nneg i32 %472 to float
  %474 = fdiv contract float %473, 2.550000e+02
  %475 = insertelement <4 x float> %471, float %474, i64 3
  br i1 %106, label %476, label %481

476:                                              ; preds = %457
  %477 = insertelement <4 x float> poison, float %470, i64 0
  %478 = insertelement <4 x float> %477, float %465, i64 1
  %479 = insertelement <4 x float> %478, float %460, i64 2
  %480 = insertelement <4 x float> %479, float %474, i64 3
  br label %481

481:                                              ; preds = %439, %453, %457, %476
  %482 = phi <4 x float> [ zeroinitializer, %439 ], [ %456, %453 ], [ %480, %476 ], [ %475, %457 ]
  %483 = fmul contract <4 x float> %111, %482
  %484 = fadd contract <4 x float> %441, %483
  %485 = fmul contract <4 x float> %114, %396
  %486 = fmul contract <4 x float> %116, %484
  %487 = fadd contract <4 x float> %485, %486
  br label %488

488:                                              ; preds = %231, %212, %208, %194, %481
  %489 = phi <4 x float> [ %487, %481 ], [ zeroinitializer, %194 ], [ %211, %208 ], [ %235, %231 ], [ %230, %212 ]
  %490 = extractelement <4 x float> %489, i64 0
  %491 = insertelement <8 x float> %156, float %490, i32 %155
  %492 = or disjoint i32 %155, 1
  %493 = extractelement <4 x float> %489, i64 1
  %494 = insertelement <8 x float> %491, float %493, i32 %492
  %495 = or disjoint i32 %155, 2
  %496 = extractelement <4 x float> %489, i64 2
  %497 = insertelement <8 x float> %494, float %496, i32 %495
  %498 = or disjoint i32 %155, 3
  %499 = extractelement <4 x float> %489, i64 3
  %500 = insertelement <8 x float> %497, float %499, i32 %498
  br i1 %154, label %153, label %139, !llvm.loop !42

501:                                              ; preds = %3, %139
  %502 = phi <4 x float> [ %152, %139 ], [ zeroinitializer, %3 ]
  ret <4 x float> %502
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(read, inaccessiblemem: none, target_mem: none) uwtable
define hidden <4 x float> @rdna4_read_2d(ptr addrspace(1) nofree noundef readonly captures(none) %0, i32 noundef %1, i32 noundef %2) local_unnamed_addr #1 {
  %4 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 8
  %5 = load i32, ptr addrspace(1) %4, align 8, !tbaa !35
  %6 = icmp ult i32 %1, %5
  br i1 %6, label %7, label %58

7:                                                ; preds = %3
  %8 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 12
  %9 = load i32, ptr addrspace(1) %8, align 4, !tbaa !37
  %10 = icmp ult i32 %2, %9
  %11 = or i32 %2, %1
  %12 = icmp sgt i32 %11, -1
  %13 = and i1 %12, %10
  br i1 %13, label %14, label %58

14:                                               ; preds = %7
  %15 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %16 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %17 = load i32, ptr addrspace(1) %16, align 8, !tbaa !39
  %18 = lshr i32 %17, 2
  %19 = mul i32 %18, %2
  %20 = add i32 %19, %1
  %21 = zext i32 %20 to i64
  %22 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %15, i64 %21
  %23 = load i32, ptr addrspace(1) %22, align 4, !tbaa !40
  %24 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %25 = load i32, ptr addrspace(1) %24, align 4, !tbaa !41
  %26 = and i32 %25, 65535
  %27 = icmp eq i32 %26, 55
  br i1 %27, label %28, label %32

28:                                               ; preds = %14
  %29 = insertelement <4 x i32> poison, i32 %23, i64 0
  %30 = bitcast <4 x i32> %29 to <4 x float>
  %31 = shufflevector <4 x float> %30, <4 x float> <float poison, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 5, i32 6, i32 7>
  br label %58

32:                                               ; preds = %14
  %33 = and i32 %23, 255
  %34 = uitofp contract nneg i32 %33 to float
  %35 = fdiv contract float %34, 2.550000e+02
  %36 = insertelement <4 x float> poison, float %35, i64 0
  %37 = lshr i32 %23, 8
  %38 = and i32 %37, 255
  %39 = uitofp contract nneg i32 %38 to float
  %40 = fdiv contract float %39, 2.550000e+02
  %41 = insertelement <4 x float> %36, float %40, i64 1
  %42 = lshr i32 %23, 16
  %43 = and i32 %42, 255
  %44 = uitofp contract nneg i32 %43 to float
  %45 = fdiv contract float %44, 2.550000e+02
  %46 = insertelement <4 x float> %41, float %45, i64 2
  %47 = lshr i32 %23, 24
  %48 = uitofp contract nneg i32 %47 to float
  %49 = fdiv contract float %48, 2.550000e+02
  %50 = insertelement <4 x float> %46, float %49, i64 3
  %51 = and i32 %25, 65534
  %52 = icmp eq i32 %51, 80
  br i1 %52, label %53, label %58

53:                                               ; preds = %32
  %54 = insertelement <4 x float> poison, float %45, i64 0
  %55 = insertelement <4 x float> %54, float %40, i64 1
  %56 = insertelement <4 x float> %55, float %35, i64 2
  %57 = insertelement <4 x float> %56, float %49, i64 3
  br label %58

58:                                               ; preds = %53, %32, %28, %3, %7
  %59 = phi <4 x float> [ zeroinitializer, %3 ], [ zeroinitializer, %7 ], [ %50, %32 ], [ %31, %28 ], [ %57, %53 ]
  ret <4 x float> %59
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(read, inaccessiblemem: none, target_mem: none) uwtable
define hidden <4 x float> @rdna4_sample_1d_array(ptr addrspace(1) nofree noundef readonly captures(none) %0, ptr addrspace(4) nofree noundef readonly captures(none) %1, float noundef %2, i32 noundef %3) local_unnamed_addr #1 {
  %5 = load i32, ptr addrspace(4) %1, align 4, !tbaa !33
  %6 = lshr i32 %5, 5
  %7 = and i32 %6, 7
  %8 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 8
  %9 = load i32, ptr addrspace(1) %8, align 8, !tbaa !35
  %10 = uitofp contract i32 %9 to float
  %11 = fmul contract float %2, %10
  %12 = icmp slt i32 %5, 0
  %13 = select contract i1 %12, float %11, float %2
  %14 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 12
  %15 = load i32, ptr addrspace(1) %14, align 4, !tbaa !37
  %16 = icmp ult i32 %3, %15
  %17 = add nsw i32 %15, -1
  %18 = select i1 %16, i32 %3, i32 %17
  %19 = icmp eq i32 %9, 0
  %20 = icmp eq i32 %15, 0
  %21 = select i1 %19, i1 true, i1 %20
  br i1 %21, label %278, label %22

22:                                               ; preds = %4
  %23 = and i32 %5, 12
  %24 = icmp eq i32 %23, 0
  br i1 %24, label %25, label %105

25:                                               ; preds = %22
  %26 = tail call contract float @llvm.floor.f32(float %13)
  %27 = fptosi float %26 to i32
  switch i32 %7, label %52 [
    i32 2, label %28
    i32 3, label %33
    i32 1, label %43
    i32 4, label %48
  ]

28:                                               ; preds = %25
  %29 = srem i32 %27, %9
  %30 = icmp slt i32 %29, 0
  %31 = select i1 %30, i32 %9, i32 0
  %32 = add nsw i32 %31, %29
  br label %57

33:                                               ; preds = %25
  %34 = shl nsw i32 %9, 1
  %35 = srem i32 %27, %34
  %36 = icmp slt i32 %35, 0
  %37 = select i1 %36, i32 %34, i32 0
  %38 = add nsw i32 %37, %35
  %39 = icmp slt i32 %38, %9
  %40 = xor i32 %38, -1
  %41 = add i32 %34, %40
  %42 = select i1 %39, i32 %38, i32 %41
  br label %57

43:                                               ; preds = %25
  %44 = ashr i32 %27, 31
  %45 = xor i32 %44, %27
  %46 = add nsw i32 %9, -1
  %47 = tail call i32 @llvm.smin.i32(i32 %45, i32 %46)
  br label %57

48:                                               ; preds = %25
  %49 = icmp sgt i32 %9, %27
  %50 = tail call i32 @llvm.smax.i32(i32 %27, i32 -1)
  %51 = select i1 %49, i32 %50, i32 -1
  br label %57

52:                                               ; preds = %25
  %53 = icmp slt i32 %27, 0
  %54 = add nsw i32 %9, -1
  %55 = tail call i32 @llvm.smin.i32(i32 %27, i32 %54)
  %56 = select i1 %53, i32 0, i32 %55
  br label %57

57:                                               ; preds = %28, %33, %43, %48, %52
  %58 = phi i32 [ %56, %52 ], [ %32, %28 ], [ %42, %33 ], [ %47, %43 ], [ %51, %48 ]
  %59 = or i32 %58, %18
  %60 = icmp sgt i32 %59, -1
  br i1 %60, label %61, label %278

61:                                               ; preds = %57
  %62 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %63 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %64 = load i32, ptr addrspace(1) %63, align 8, !tbaa !39
  %65 = lshr i32 %64, 2
  %66 = mul i32 %65, %18
  %67 = add i32 %66, %58
  %68 = zext i32 %67 to i64
  %69 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %62, i64 %68
  %70 = load i32, ptr addrspace(1) %69, align 4, !tbaa !40
  %71 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %72 = load i32, ptr addrspace(1) %71, align 4, !tbaa !41
  %73 = and i32 %72, 65535
  %74 = icmp eq i32 %73, 55
  br i1 %74, label %75, label %79

75:                                               ; preds = %61
  %76 = insertelement <4 x i32> poison, i32 %70, i64 0
  %77 = bitcast <4 x i32> %76 to <4 x float>
  %78 = shufflevector <4 x float> %77, <4 x float> <float poison, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 5, i32 6, i32 7>
  br label %278

79:                                               ; preds = %61
  %80 = and i32 %70, 255
  %81 = uitofp contract nneg i32 %80 to float
  %82 = fdiv contract float %81, 2.550000e+02
  %83 = insertelement <4 x float> poison, float %82, i64 0
  %84 = lshr i32 %70, 8
  %85 = and i32 %84, 255
  %86 = uitofp contract nneg i32 %85 to float
  %87 = fdiv contract float %86, 2.550000e+02
  %88 = insertelement <4 x float> %83, float %87, i64 1
  %89 = lshr i32 %70, 16
  %90 = and i32 %89, 255
  %91 = uitofp contract nneg i32 %90 to float
  %92 = fdiv contract float %91, 2.550000e+02
  %93 = insertelement <4 x float> %88, float %92, i64 2
  %94 = lshr i32 %70, 24
  %95 = uitofp contract nneg i32 %94 to float
  %96 = fdiv contract float %95, 2.550000e+02
  %97 = insertelement <4 x float> %93, float %96, i64 3
  %98 = and i32 %72, 65534
  %99 = icmp eq i32 %98, 80
  br i1 %99, label %100, label %278

100:                                              ; preds = %79
  %101 = insertelement <4 x float> poison, float %92, i64 0
  %102 = insertelement <4 x float> %101, float %87, i64 1
  %103 = insertelement <4 x float> %102, float %82, i64 2
  %104 = insertelement <4 x float> %103, float %96, i64 3
  br label %278

105:                                              ; preds = %22
  %106 = fadd contract float %13, -5.000000e-01
  %107 = tail call contract float @llvm.floor.f32(float %106)
  %108 = fptosi float %107 to i32
  %109 = sitofp contract i32 %108 to float
  %110 = fsub contract float %106, %109
  switch i32 %7, label %135 [
    i32 2, label %111
    i32 3, label %116
    i32 1, label %126
    i32 4, label %131
  ]

111:                                              ; preds = %105
  %112 = srem i32 %108, %9
  %113 = icmp slt i32 %112, 0
  %114 = select i1 %113, i32 %9, i32 0
  %115 = add nsw i32 %114, %112
  br label %140

116:                                              ; preds = %105
  %117 = shl nsw i32 %9, 1
  %118 = srem i32 %108, %117
  %119 = icmp slt i32 %118, 0
  %120 = select i1 %119, i32 %117, i32 0
  %121 = add nsw i32 %120, %118
  %122 = icmp slt i32 %121, %9
  %123 = xor i32 %121, -1
  %124 = add i32 %117, %123
  %125 = select i1 %122, i32 %121, i32 %124
  br label %140

126:                                              ; preds = %105
  %127 = ashr i32 %108, 31
  %128 = xor i32 %127, %108
  %129 = add nsw i32 %9, -1
  %130 = tail call i32 @llvm.smin.i32(i32 %128, i32 %129)
  br label %140

131:                                              ; preds = %105
  %132 = icmp sgt i32 %9, %108
  %133 = tail call i32 @llvm.smax.i32(i32 %108, i32 -1)
  %134 = select i1 %132, i32 %133, i32 -1
  br label %140

135:                                              ; preds = %105
  %136 = icmp slt i32 %108, 0
  %137 = add nsw i32 %9, -1
  %138 = tail call i32 @llvm.smin.i32(i32 %108, i32 %137)
  %139 = select i1 %136, i32 0, i32 %138
  br label %140

140:                                              ; preds = %111, %116, %126, %131, %135
  %141 = phi i32 [ %139, %135 ], [ %115, %111 ], [ %125, %116 ], [ %130, %126 ], [ %134, %131 ]
  %142 = or i32 %141, %18
  %143 = icmp sgt i32 %142, -1
  br i1 %143, label %144, label %188

144:                                              ; preds = %140
  %145 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %146 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %147 = load i32, ptr addrspace(1) %146, align 8, !tbaa !39
  %148 = lshr i32 %147, 2
  %149 = mul i32 %148, %18
  %150 = add i32 %149, %141
  %151 = zext i32 %150 to i64
  %152 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %145, i64 %151
  %153 = load i32, ptr addrspace(1) %152, align 4, !tbaa !40
  %154 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %155 = load i32, ptr addrspace(1) %154, align 4, !tbaa !41
  %156 = and i32 %155, 65535
  %157 = icmp eq i32 %156, 55
  br i1 %157, label %158, label %162

158:                                              ; preds = %144
  %159 = insertelement <4 x i32> poison, i32 %153, i64 0
  %160 = bitcast <4 x i32> %159 to <4 x float>
  %161 = shufflevector <4 x float> %160, <4 x float> <float poison, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 5, i32 6, i32 7>
  br label %188

162:                                              ; preds = %144
  %163 = and i32 %153, 255
  %164 = uitofp contract nneg i32 %163 to float
  %165 = fdiv contract float %164, 2.550000e+02
  %166 = insertelement <4 x float> poison, float %165, i64 0
  %167 = lshr i32 %153, 8
  %168 = and i32 %167, 255
  %169 = uitofp contract nneg i32 %168 to float
  %170 = fdiv contract float %169, 2.550000e+02
  %171 = insertelement <4 x float> %166, float %170, i64 1
  %172 = lshr i32 %153, 16
  %173 = and i32 %172, 255
  %174 = uitofp contract nneg i32 %173 to float
  %175 = fdiv contract float %174, 2.550000e+02
  %176 = insertelement <4 x float> %171, float %175, i64 2
  %177 = lshr i32 %153, 24
  %178 = uitofp contract nneg i32 %177 to float
  %179 = fdiv contract float %178, 2.550000e+02
  %180 = insertelement <4 x float> %176, float %179, i64 3
  %181 = and i32 %155, 65534
  %182 = icmp eq i32 %181, 80
  br i1 %182, label %183, label %188

183:                                              ; preds = %162
  %184 = insertelement <4 x float> poison, float %175, i64 0
  %185 = insertelement <4 x float> %184, float %170, i64 1
  %186 = insertelement <4 x float> %185, float %165, i64 2
  %187 = insertelement <4 x float> %186, float %179, i64 3
  br label %188

188:                                              ; preds = %140, %158, %162, %183
  %189 = phi <4 x float> [ zeroinitializer, %140 ], [ %161, %158 ], [ %187, %183 ], [ %180, %162 ]
  %190 = fsub contract float 1.000000e+00, %110
  %191 = insertelement <4 x float> poison, float %190, i64 0
  %192 = shufflevector <4 x float> %191, <4 x float> poison, <4 x i32> zeroinitializer
  %193 = fmul contract <4 x float> %192, %189
  %194 = add nsw i32 %108, 1
  switch i32 %7, label %219 [
    i32 2, label %195
    i32 3, label %200
    i32 1, label %210
    i32 4, label %215
  ]

195:                                              ; preds = %188
  %196 = srem i32 %194, %9
  %197 = icmp slt i32 %196, 0
  %198 = select i1 %197, i32 %9, i32 0
  %199 = add nsw i32 %198, %196
  br label %224

200:                                              ; preds = %188
  %201 = shl nsw i32 %9, 1
  %202 = srem i32 %194, %201
  %203 = icmp slt i32 %202, 0
  %204 = select i1 %203, i32 %201, i32 0
  %205 = add nsw i32 %204, %202
  %206 = icmp slt i32 %205, %9
  %207 = xor i32 %205, -1
  %208 = add i32 %201, %207
  %209 = select i1 %206, i32 %205, i32 %208
  br label %224

210:                                              ; preds = %188
  %211 = ashr i32 %194, 31
  %212 = xor i32 %211, %194
  %213 = add nsw i32 %9, -1
  %214 = tail call i32 @llvm.smin.i32(i32 %212, i32 %213)
  br label %224

215:                                              ; preds = %188
  %216 = icmp slt i32 %194, %9
  %217 = tail call i32 @llvm.smax.i32(i32 %194, i32 -1)
  %218 = select i1 %216, i32 %217, i32 -1
  br label %224

219:                                              ; preds = %188
  %220 = icmp slt i32 %108, -1
  %221 = add nsw i32 %9, -1
  %222 = tail call i32 @llvm.smin.i32(i32 %194, i32 %221)
  %223 = select i1 %220, i32 0, i32 %222
  br label %224

224:                                              ; preds = %195, %200, %210, %215, %219
  %225 = phi i32 [ %223, %219 ], [ %199, %195 ], [ %209, %200 ], [ %214, %210 ], [ %218, %215 ]
  %226 = or i32 %225, %18
  %227 = icmp sgt i32 %226, -1
  br i1 %227, label %228, label %272

228:                                              ; preds = %224
  %229 = load ptr addrspace(1), ptr addrspace(1) %0, align 8, !tbaa !38
  %230 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 16
  %231 = load i32, ptr addrspace(1) %230, align 8, !tbaa !39
  %232 = lshr i32 %231, 2
  %233 = mul i32 %232, %18
  %234 = add i32 %233, %225
  %235 = zext i32 %234 to i64
  %236 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %229, i64 %235
  %237 = load i32, ptr addrspace(1) %236, align 4, !tbaa !40
  %238 = getelementptr inbounds nuw i8, ptr addrspace(1) %0, i64 20
  %239 = load i32, ptr addrspace(1) %238, align 4, !tbaa !41
  %240 = and i32 %239, 65535
  %241 = icmp eq i32 %240, 55
  br i1 %241, label %242, label %246

242:                                              ; preds = %228
  %243 = insertelement <4 x i32> poison, i32 %237, i64 0
  %244 = bitcast <4 x i32> %243 to <4 x float>
  %245 = shufflevector <4 x float> %244, <4 x float> <float poison, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 5, i32 6, i32 7>
  br label %272

246:                                              ; preds = %228
  %247 = and i32 %237, 255
  %248 = uitofp contract nneg i32 %247 to float
  %249 = fdiv contract float %248, 2.550000e+02
  %250 = insertelement <4 x float> poison, float %249, i64 0
  %251 = lshr i32 %237, 8
  %252 = and i32 %251, 255
  %253 = uitofp contract nneg i32 %252 to float
  %254 = fdiv contract float %253, 2.550000e+02
  %255 = insertelement <4 x float> %250, float %254, i64 1
  %256 = lshr i32 %237, 16
  %257 = and i32 %256, 255
  %258 = uitofp contract nneg i32 %257 to float
  %259 = fdiv contract float %258, 2.550000e+02
  %260 = insertelement <4 x float> %255, float %259, i64 2
  %261 = lshr i32 %237, 24
  %262 = uitofp contract nneg i32 %261 to float
  %263 = fdiv contract float %262, 2.550000e+02
  %264 = insertelement <4 x float> %260, float %263, i64 3
  %265 = and i32 %239, 65534
  %266 = icmp eq i32 %265, 80
  br i1 %266, label %267, label %272

267:                                              ; preds = %246
  %268 = insertelement <4 x float> poison, float %259, i64 0
  %269 = insertelement <4 x float> %268, float %254, i64 1
  %270 = insertelement <4 x float> %269, float %249, i64 2
  %271 = insertelement <4 x float> %270, float %263, i64 3
  br label %272

272:                                              ; preds = %224, %242, %246, %267
  %273 = phi <4 x float> [ zeroinitializer, %224 ], [ %245, %242 ], [ %271, %267 ], [ %264, %246 ]
  %274 = insertelement <4 x float> poison, float %110, i64 0
  %275 = shufflevector <4 x float> %274, <4 x float> poison, <4 x i32> zeroinitializer
  %276 = fmul contract <4 x float> %275, %273
  %277 = fadd contract <4 x float> %193, %276
  br label %278

278:                                              ; preds = %100, %79, %75, %57, %4, %272
  %279 = phi <4 x float> [ zeroinitializer, %4 ], [ %277, %272 ], [ zeroinitializer, %57 ], [ %78, %75 ], [ %104, %100 ], [ %97, %79 ]
  ret <4 x float> %279
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(read, argmem: readwrite, inaccessiblemem: none, target_mem: none) uwtable
define hidden range(i32 0, 2) i32 @rdna4_px_begin(ptr nofree noundef readonly captures(none) %0, i32 noundef %1, i32 noundef %2, ptr nofree noundef writeonly captures(none) initializes((0, 8), (16, 36)) %3) local_unnamed_addr #4 {
  store i32 %1, ptr %3, align 16, !tbaa !44
  %5 = getelementptr inbounds nuw i8, ptr %3, i64 4
  store i32 %2, ptr %5, align 4, !tbaa !46
  %6 = getelementptr inbounds nuw i8, ptr %3, i64 16
  store <4 x float> zeroinitializer, ptr %6, align 16, !tbaa !47
  %7 = getelementptr inbounds nuw i8, ptr %3, i64 32
  store i32 0, ptr %7, align 16, !tbaa !48
  %8 = getelementptr inbounds nuw i8, ptr %0, i64 36
  %9 = load i32, ptr %8, align 4, !tbaa !21
  %10 = icmp ult i32 %1, %9
  br i1 %10, label %11, label %75

11:                                               ; preds = %4
  %12 = getelementptr inbounds nuw i8, ptr %0, i64 40
  %13 = load i32, ptr %12, align 8, !tbaa !22
  %14 = icmp ult i32 %2, %13
  br i1 %14, label %15, label %75

15:                                               ; preds = %11
  %16 = getelementptr inbounds nuw i8, ptr %0, i64 68
  %17 = load i32, ptr %16, align 4, !tbaa !29
  %18 = sub i32 %1, %17
  %19 = getelementptr inbounds nuw i8, ptr %0, i64 76
  %20 = load i32, ptr %19, align 4, !tbaa !31
  %21 = icmp ult i32 %18, %20
  br i1 %21, label %22, label %75

22:                                               ; preds = %15
  %23 = getelementptr inbounds nuw i8, ptr %0, i64 72
  %24 = load i32, ptr %23, align 8, !tbaa !30
  %25 = sub i32 %2, %24
  %26 = getelementptr inbounds nuw i8, ptr %0, i64 80
  %27 = load i32, ptr %26, align 8, !tbaa !32
  %28 = icmp ult i32 %25, %27
  br i1 %28, label %29, label %75

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
  %42 = icmp eq i32 %41, 55
  br i1 %42, label %43, label %47

43:                                               ; preds = %29
  %44 = insertelement <4 x i32> poison, i32 %39, i64 0
  %45 = bitcast <4 x i32> %44 to <4 x float>
  %46 = shufflevector <4 x float> %45, <4 x float> <float poison, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 5, i32 6, i32 7>
  br label %73

47:                                               ; preds = %29
  %48 = and i32 %39, 255
  %49 = uitofp contract nneg i32 %48 to float
  %50 = fdiv contract float %49, 2.550000e+02
  %51 = insertelement <4 x float> poison, float %50, i64 0
  %52 = lshr i32 %39, 8
  %53 = and i32 %52, 255
  %54 = uitofp contract nneg i32 %53 to float
  %55 = fdiv contract float %54, 2.550000e+02
  %56 = insertelement <4 x float> %51, float %55, i64 1
  %57 = lshr i32 %39, 16
  %58 = and i32 %57, 255
  %59 = uitofp contract nneg i32 %58 to float
  %60 = fdiv contract float %59, 2.550000e+02
  %61 = insertelement <4 x float> %56, float %60, i64 2
  %62 = lshr i32 %39, 24
  %63 = uitofp contract nneg i32 %62 to float
  %64 = fdiv contract float %63, 2.550000e+02
  %65 = insertelement <4 x float> %61, float %64, i64 3
  %66 = and i32 %41, -2
  %67 = icmp eq i32 %66, 80
  br i1 %67, label %68, label %73

68:                                               ; preds = %47
  %69 = insertelement <4 x float> poison, float %60, i64 0
  %70 = insertelement <4 x float> %69, float %55, i64 1
  %71 = insertelement <4 x float> %70, float %50, i64 2
  %72 = insertelement <4 x float> %71, float %64, i64 3
  br label %73

73:                                               ; preds = %43, %47, %68
  %74 = phi <4 x float> [ %46, %43 ], [ %72, %68 ], [ %65, %47 ]
  store <4 x float> %74, ptr %6, align 16, !tbaa !47
  br label %75

75:                                               ; preds = %4, %11, %15, %22, %73
  %76 = phi i32 [ 1, %73 ], [ 0, %22 ], [ 0, %15 ], [ 0, %11 ], [ 0, %4 ]
  ret i32 %76
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: read) uwtable
define hidden range(i32 0, -2) i32 @rdna4_tri_count(ptr nofree noundef readonly captures(none) %0) local_unnamed_addr #5 {
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
define hidden range(i32 0, 3) i32 @rdna4_tri_cover(ptr nofree noundef readonly captures(none) %0, ptr nofree noundef readonly captures(none) %1, i32 noundef %2, ptr nofree noundef writeonly captures(none) initializes((0, 24)) %3) local_unnamed_addr #4 {
  %5 = mul i32 %2, 3
  %6 = add i32 %5, 1
  %7 = load i32, ptr %1, align 16, !tbaa !44
  %8 = uitofp contract i32 %7 to float
  %9 = fadd contract float %8, 5.000000e-01
  %10 = getelementptr inbounds nuw i8, ptr %1, i64 4
  %11 = load i32, ptr %10, align 4, !tbaa !46
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
  store ptr addrspace(1) %29, ptr %3, align 8, !tbaa !49
  %30 = load ptr addrspace(1), ptr %0, align 8, !tbaa !9
  %31 = mul i32 %21, %26
  %32 = zext i32 %31 to i64
  %33 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %30, i64 %32
  %34 = getelementptr inbounds nuw i8, ptr %3, i64 8
  store ptr addrspace(1) %33, ptr %34, align 8, !tbaa !49
  %35 = load ptr addrspace(1), ptr %0, align 8, !tbaa !9
  %36 = mul i32 %23, %26
  %37 = zext i32 %36 to i64
  %38 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %35, i64 %37
  %39 = getelementptr inbounds nuw i8, ptr %3, i64 16
  store ptr addrspace(1) %38, ptr %39, align 8, !tbaa !49
  %40 = getelementptr inbounds nuw i8, ptr %0, i64 12
  %41 = load i32, ptr %40, align 4, !tbaa !16
  %42 = add i32 %41, 3
  %43 = zext i32 %42 to i64
  %44 = zext i32 %41 to i64
  %45 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %29, i64 %43
  %46 = load float, ptr addrspace(1) %45, align 4, !tbaa !50
  %47 = fcmp contract ugt float %46, 0.000000e+00
  br i1 %47, label %48, label %186

48:                                               ; preds = %4
  %49 = getelementptr inbounds nuw i8, ptr %0, i64 60
  %50 = getelementptr inbounds nuw i8, ptr %0, i64 52
  %51 = getelementptr inbounds nuw i8, ptr %0, i64 56
  %52 = getelementptr inbounds nuw i8, ptr %0, i64 48
  %53 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %29, i64 %44
  %54 = getelementptr inbounds nuw i8, ptr addrspace(1) %53, i64 12
  %55 = load float, ptr addrspace(1) %54, align 4, !tbaa !50
  %56 = fdiv contract float 1.000000e+00, %55
  %57 = load float, ptr %52, align 8, !tbaa !24
  %58 = load float, ptr addrspace(1) %53, align 4, !tbaa !50
  %59 = fmul contract float %56, %58
  %60 = fmul contract float %59, 5.000000e-01
  %61 = fadd contract float %60, 5.000000e-01
  %62 = load float, ptr %51, align 8, !tbaa !26
  %63 = fmul contract float %62, %61
  %64 = fadd contract float %57, %63
  %65 = load float, ptr %50, align 4, !tbaa !25
  %66 = getelementptr inbounds nuw i8, ptr addrspace(1) %53, i64 4
  %67 = load float, ptr addrspace(1) %66, align 4, !tbaa !50
  %68 = fmul contract float %56, %67
  %69 = fmul contract float %68, 5.000000e-01
  %70 = fsub contract float 5.000000e-01, %69
  %71 = load float, ptr %49, align 4, !tbaa !27
  %72 = fmul contract float %71, %70
  %73 = fadd contract float %65, %72
  %74 = getelementptr inbounds nuw i8, ptr addrspace(1) %53, i64 8
  %75 = load float, ptr addrspace(1) %74, align 4, !tbaa !50
  %76 = fmul contract float %56, %75
  %77 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %33, i64 %43
  %78 = load float, ptr addrspace(1) %77, align 4, !tbaa !50
  %79 = fcmp contract ugt float %78, 0.000000e+00
  br i1 %79, label %80, label %186

80:                                               ; preds = %48
  %81 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %33, i64 %44
  %82 = getelementptr inbounds nuw i8, ptr addrspace(1) %81, i64 12
  %83 = load float, ptr addrspace(1) %82, align 4, !tbaa !50
  %84 = fdiv contract float 1.000000e+00, %83
  %85 = load float, ptr addrspace(1) %81, align 4, !tbaa !50
  %86 = fmul contract float %84, %85
  %87 = fmul contract float %86, 5.000000e-01
  %88 = fadd contract float %87, 5.000000e-01
  %89 = fmul contract float %62, %88
  %90 = fadd contract float %57, %89
  %91 = getelementptr inbounds nuw i8, ptr addrspace(1) %81, i64 4
  %92 = load float, ptr addrspace(1) %91, align 4, !tbaa !50
  %93 = fmul contract float %84, %92
  %94 = fmul contract float %93, 5.000000e-01
  %95 = fsub contract float 5.000000e-01, %94
  %96 = fmul contract float %71, %95
  %97 = fadd contract float %65, %96
  %98 = getelementptr inbounds nuw i8, ptr addrspace(1) %81, i64 8
  %99 = load float, ptr addrspace(1) %98, align 4, !tbaa !50
  %100 = fmul contract float %84, %99
  %101 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %38, i64 %43
  %102 = load float, ptr addrspace(1) %101, align 4, !tbaa !50
  %103 = fcmp contract ugt float %102, 0.000000e+00
  br i1 %103, label %104, label %186

104:                                              ; preds = %80
  %105 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %38, i64 %44
  %106 = getelementptr inbounds nuw i8, ptr addrspace(1) %105, i64 12
  %107 = load float, ptr addrspace(1) %106, align 4, !tbaa !50
  %108 = fdiv contract float 1.000000e+00, %107
  %109 = load float, ptr addrspace(1) %105, align 4, !tbaa !50
  %110 = fmul contract float %108, %109
  %111 = fmul contract float %110, 5.000000e-01
  %112 = fadd contract float %111, 5.000000e-01
  %113 = fmul contract float %62, %112
  %114 = fadd contract float %57, %113
  %115 = getelementptr inbounds nuw i8, ptr addrspace(1) %105, i64 4
  %116 = load float, ptr addrspace(1) %115, align 4, !tbaa !50
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
  %132 = load float, ptr addrspace(1) %131, align 4, !tbaa !50
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
  store float %153, ptr %161, align 4, !tbaa !50
  %163 = fdiv contract float %156, %160
  store float %163, ptr %162, align 8, !tbaa !50
  %164 = getelementptr inbounds nuw i8, ptr %3, i64 40
  store float %154, ptr %164, align 8, !tbaa !50
  %165 = fdiv contract float %157, %160
  %166 = getelementptr inbounds nuw i8, ptr %3, i64 28
  store float %165, ptr %166, align 4, !tbaa !50
  %167 = getelementptr inbounds nuw i8, ptr %3, i64 44
  store float %155, ptr %167, align 4, !tbaa !50
  %168 = fdiv contract float %159, %160
  %169 = getelementptr inbounds nuw i8, ptr %3, i64 32
  store float %168, ptr %169, align 8, !tbaa !50
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
  store float %179, ptr %180, align 8, !tbaa !51
  %181 = getelementptr inbounds nuw i8, ptr %3, i64 52
  store float %160, ptr %181, align 4, !tbaa !53
  %182 = fcmp contract olt float %128, 0.000000e+00
  %183 = zext i1 %182 to i32
  %184 = getelementptr inbounds nuw i8, ptr %3, i64 56
  store i32 %183, ptr %184, align 8, !tbaa !54
  %185 = select i1 %174, i32 2, i32 1
  br label %186

186:                                              ; preds = %4, %48, %80, %104, %130
  %187 = phi i32 [ 0, %104 ], [ %185, %130 ], [ 0, %80 ], [ 0, %48 ], [ 0, %4 ]
  ret i32 %187
}

; Function Attrs: alwaysinline nofree norecurse nosync nounwind memory(read, inaccessiblemem: none, target_mem: none) uwtable
define hidden <4 x float> @rdna4_interp(ptr nofree noundef readonly captures(none) %0, i32 noundef %1, i32 noundef %2, i32 noundef %3) local_unnamed_addr #3 {
  %5 = tail call i32 @llvm.umin.i32(i32 %2, i32 4)
  %6 = icmp eq i32 %2, 0
  br i1 %6, label %19, label %7

7:                                                ; preds = %4
  %8 = load ptr addrspace(1), ptr %0, align 8, !tbaa !49
  %9 = getelementptr inbounds nuw i8, ptr %0, i64 8
  %10 = load ptr addrspace(1), ptr %9, align 8, !tbaa !49
  %11 = getelementptr inbounds nuw i8, ptr %0, i64 16
  %12 = load ptr addrspace(1), ptr %11, align 8, !tbaa !49
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
  %27 = load float, ptr addrspace(1) %26, align 4, !tbaa !50
  %28 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %10, i64 %25
  %29 = load float, ptr addrspace(1) %28, align 4, !tbaa !50
  %30 = getelementptr inbounds nuw [4 x i8], ptr addrspace(1) %12, i64 %25
  %31 = load float, ptr addrspace(1) %30, align 4, !tbaa !50
  switch i32 %3, label %41 [
    i32 2, label %50
    i32 1, label %32
  ]

32:                                               ; preds = %21
  %33 = load float, ptr %13, align 4, !tbaa !50
  %34 = fmul contract float %27, %33
  %35 = load float, ptr %14, align 8, !tbaa !50
  %36 = fmul contract float %29, %35
  %37 = fadd contract float %34, %36
  %38 = load float, ptr %15, align 4, !tbaa !50
  %39 = fmul contract float %31, %38
  %40 = fadd contract float %37, %39
  br label %50

41:                                               ; preds = %21
  %42 = load float, ptr %16, align 8, !tbaa !50
  %43 = fmul contract float %27, %42
  %44 = load float, ptr %17, align 4, !tbaa !50
  %45 = fmul contract float %29, %44
  %46 = fadd contract float %43, %45
  %47 = load float, ptr %18, align 8, !tbaa !50
  %48 = fmul contract float %31, %47
  %49 = fadd contract float %46, %48
  br label %50

50:                                               ; preds = %21, %32, %41
  %51 = phi float [ %49, %41 ], [ %40, %32 ], [ %27, %21 ]
  %52 = insertelement <4 x float> %22, float %51, i32 %23
  %53 = add nuw nsw i32 %23, 1
  %54 = icmp eq i32 %53, %5
  br i1 %54, label %19, label %21, !llvm.loop !55
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: read) uwtable
define hidden <4 x float> @rdna4_frag_position(ptr nofree noundef readonly captures(none) %0, ptr nofree noundef readonly captures(none) %1) local_unnamed_addr #5 {
  %3 = load i32, ptr %0, align 16, !tbaa !44
  %4 = uitofp contract i32 %3 to float
  %5 = fadd contract float %4, 5.000000e-01
  %6 = insertelement <4 x float> poison, float %5, i64 0
  %7 = getelementptr inbounds nuw i8, ptr %0, i64 4
  %8 = load i32, ptr %7, align 4, !tbaa !46
  %9 = uitofp contract i32 %8 to float
  %10 = fadd contract float %9, 5.000000e-01
  %11 = insertelement <4 x float> %6, float %10, i64 1
  %12 = getelementptr inbounds nuw i8, ptr %1, i64 48
  %13 = load float, ptr %12, align 8, !tbaa !51
  %14 = insertelement <4 x float> %11, float %13, i64 2
  %15 = getelementptr inbounds nuw i8, ptr %1, i64 52
  %16 = load float, ptr %15, align 4, !tbaa !53
  %17 = insertelement <4 x float> %14, float %16, i64 3
  ret <4 x float> %17
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: read) uwtable
define hidden i32 @rdna4_front_facing(ptr nofree noundef readonly captures(none) %0) local_unnamed_addr #5 {
  %2 = getelementptr inbounds nuw i8, ptr %0, i64 56
  %3 = load i32, ptr %2, align 8, !tbaa !54
  ret i32 %3
}

; Function Attrs: alwaysinline nofree norecurse nosync nounwind memory(argmem: readwrite) uwtable
define hidden void @rdna4_px_blend(ptr nofree noundef readonly captures(none) %0, ptr nofree noundef captures(none) initializes((32, 36)) %1, <4 x float> noundef %2) local_unnamed_addr #6 {
  %4 = getelementptr inbounds nuw i8, ptr %1, i64 32
  store i32 1, ptr %4, align 16, !tbaa !48
  %5 = getelementptr inbounds nuw i8, ptr %0, i64 64
  %6 = load i32, ptr %5, align 8, !tbaa !28
  %7 = icmp sgt i32 %6, -1
  %8 = getelementptr inbounds nuw i8, ptr %1, i64 16
  br i1 %7, label %168, label %9

9:                                                ; preds = %3
  %10 = load <4 x float>, ptr %8, align 16, !tbaa !47
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
  store <4 x float> %169, ptr %8, align 16, !tbaa !47
  ret void
}

; Function Attrs: alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(write, argmem: readwrite, inaccessiblemem: none, target_mem: none) uwtable
define hidden void @rdna4_px_end(ptr nofree noundef readonly captures(none) %0, ptr nofree noundef readonly captures(none) %1) local_unnamed_addr #7 {
  %3 = getelementptr inbounds nuw i8, ptr %1, i64 32
  %4 = load i32, ptr %3, align 16, !tbaa !48
  %5 = icmp eq i32 %4, 0
  br i1 %5, label %65, label %6

6:                                                ; preds = %2
  %7 = getelementptr inbounds nuw i8, ptr %1, i64 16
  %8 = load <4 x float>, ptr %7, align 16, !tbaa !47
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
  %56 = load i32, ptr %55, align 4, !tbaa !46
  %57 = getelementptr inbounds nuw i8, ptr %0, i64 32
  %58 = load i32, ptr %57, align 8, !tbaa !20
  %59 = lshr i32 %58, 2
  %60 = mul i32 %59, %56
  %61 = load i32, ptr %1, align 16, !tbaa !44
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
declare i32 @llvm.umax.i32(i32, i32) #8

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare i32 @llvm.usub.sat.i32(i32, i32) #8

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare i32 @llvm.umin.i32(i32, i32) #8

attributes #0 = { alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: write) uwtable "amdgpu-no-cluster-id-x" "amdgpu-no-cluster-id-y" "amdgpu-no-cluster-id-z" "amdgpu-no-completion-action" "amdgpu-no-default-queue" "amdgpu-no-dispatch-id" "amdgpu-no-dispatch-ptr" "amdgpu-no-flat-scratch-init" "amdgpu-no-heap-ptr" "amdgpu-no-hostcall-ptr" "amdgpu-no-implicitarg-ptr" "amdgpu-no-lds-kernel-id" "amdgpu-no-multigrid-sync-arg" "amdgpu-no-queue-ptr" "amdgpu-no-workgroup-id-x" "amdgpu-no-workgroup-id-y" "amdgpu-no-workgroup-id-z" "amdgpu-no-workitem-id-x" "amdgpu-no-workitem-id-y" "amdgpu-no-workitem-id-z" "amdgpu-no-wwm" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="gfx1201" }
attributes #1 = { alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(read, inaccessiblemem: none, target_mem: none) uwtable "amdgpu-no-cluster-id-x" "amdgpu-no-cluster-id-y" "amdgpu-no-cluster-id-z" "amdgpu-no-completion-action" "amdgpu-no-default-queue" "amdgpu-no-dispatch-id" "amdgpu-no-dispatch-ptr" "amdgpu-no-flat-scratch-init" "amdgpu-no-heap-ptr" "amdgpu-no-hostcall-ptr" "amdgpu-no-implicitarg-ptr" "amdgpu-no-lds-kernel-id" "amdgpu-no-multigrid-sync-arg" "amdgpu-no-queue-ptr" "amdgpu-no-workgroup-id-x" "amdgpu-no-workgroup-id-y" "amdgpu-no-workgroup-id-z" "amdgpu-no-workitem-id-x" "amdgpu-no-workitem-id-y" "amdgpu-no-workitem-id-z" "amdgpu-no-wwm" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="gfx1201" }
attributes #2 = { mustprogress nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }
attributes #3 = { alwaysinline nofree norecurse nosync nounwind memory(read, inaccessiblemem: none, target_mem: none) uwtable "amdgpu-no-cluster-id-x" "amdgpu-no-cluster-id-y" "amdgpu-no-cluster-id-z" "amdgpu-no-completion-action" "amdgpu-no-default-queue" "amdgpu-no-dispatch-id" "amdgpu-no-dispatch-ptr" "amdgpu-no-flat-scratch-init" "amdgpu-no-heap-ptr" "amdgpu-no-hostcall-ptr" "amdgpu-no-implicitarg-ptr" "amdgpu-no-lds-kernel-id" "amdgpu-no-multigrid-sync-arg" "amdgpu-no-queue-ptr" "amdgpu-no-workgroup-id-x" "amdgpu-no-workgroup-id-y" "amdgpu-no-workgroup-id-z" "amdgpu-no-workitem-id-x" "amdgpu-no-workitem-id-y" "amdgpu-no-workitem-id-z" "amdgpu-no-wwm" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="gfx1201" }
attributes #4 = { alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(read, argmem: readwrite, inaccessiblemem: none, target_mem: none) uwtable "amdgpu-no-cluster-id-x" "amdgpu-no-cluster-id-y" "amdgpu-no-cluster-id-z" "amdgpu-no-completion-action" "amdgpu-no-default-queue" "amdgpu-no-dispatch-id" "amdgpu-no-dispatch-ptr" "amdgpu-no-flat-scratch-init" "amdgpu-no-heap-ptr" "amdgpu-no-hostcall-ptr" "amdgpu-no-implicitarg-ptr" "amdgpu-no-lds-kernel-id" "amdgpu-no-multigrid-sync-arg" "amdgpu-no-queue-ptr" "amdgpu-no-workgroup-id-x" "amdgpu-no-workgroup-id-y" "amdgpu-no-workgroup-id-z" "amdgpu-no-workitem-id-x" "amdgpu-no-workitem-id-y" "amdgpu-no-workitem-id-z" "amdgpu-no-wwm" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="gfx1201" }
attributes #5 = { alwaysinline mustprogress nofree norecurse nosync nounwind willreturn memory(argmem: read) uwtable "amdgpu-no-cluster-id-x" "amdgpu-no-cluster-id-y" "amdgpu-no-cluster-id-z" "amdgpu-no-completion-action" "amdgpu-no-default-queue" "amdgpu-no-dispatch-id" "amdgpu-no-dispatch-ptr" "amdgpu-no-flat-scratch-init" "amdgpu-no-heap-ptr" "amdgpu-no-hostcall-ptr" "amdgpu-no-implicitarg-ptr" "amdgpu-no-lds-kernel-id" "amdgpu-no-multigrid-sync-arg" "amdgpu-no-queue-ptr" "amdgpu-no-workgroup-id-x" "amdgpu-no-workgroup-id-y" "amdgpu-no-workgroup-id-z" "amdgpu-no-workitem-id-x" "amdgpu-no-workitem-id-y" "amdgpu-no-workitem-id-z" "amdgpu-no-wwm" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="gfx1201" }
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
!42 = distinct !{!42, !43}
!43 = !{!"llvm.loop.mustprogress"}
!44 = !{!45, !6, i64 0}
!45 = !{!"rdna4_px", !6, i64 0, !6, i64 4, !7, i64 16, !6, i64 32}
!46 = !{!45, !6, i64 4}
!47 = !{!7, !7, i64 0}
!48 = !{!45, !6, i64 32}
!49 = !{!11, !11, i64 0}
!50 = !{!14, !14, i64 0}
!51 = !{!52, !14, i64 48}
!52 = !{!"rdna4_tri", !7, i64 0, !7, i64 24, !7, i64 36, !14, i64 48, !14, i64 52, !6, i64 56}
!53 = !{!52, !14, i64 52}
!54 = !{!52, !6, i64 56}
!55 = distinct !{!55, !43}

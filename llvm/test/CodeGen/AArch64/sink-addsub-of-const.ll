; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mtriple=aarch64-unknown-unknown | FileCheck %s

; Scalar tests. Trying to avoid LEA here, so the output is actually readable..

define i32 @sink_add_of_const_to_add(i32 %a, i32 %b, i32 %c) {
; CHECK-LABEL: sink_add_of_const_to_add:
; CHECK:       // %bb.0:
; CHECK-NEXT:    add w8, w0, w1
; CHECK-NEXT:    add w8, w8, w2
; CHECK-NEXT:    add w0, w8, #32 // =32
; CHECK-NEXT:    ret
  %t0 = add i32 %a, %b
  %t1 = add i32 %t0, 32
  %r = add i32 %t1, %c
  ret i32 %r
}
define i32 @sink_sub_of_const_to_add(i32 %a, i32 %b, i32 %c) {
; CHECK-LABEL: sink_sub_of_const_to_add:
; CHECK:       // %bb.0:
; CHECK-NEXT:    add w8, w0, w1
; CHECK-NEXT:    add w8, w8, w2
; CHECK-NEXT:    sub w0, w8, #32 // =32
; CHECK-NEXT:    ret
  %t0 = add i32 %a, %b
  %t1 = sub i32 %t0, 32
  %r = add i32 %t1, %c
  ret i32 %r
}

define i32 @sink_add_of_const_to_sub(i32 %a, i32 %b, i32 %c) {
; CHECK-LABEL: sink_add_of_const_to_sub:
; CHECK:       // %bb.0:
; CHECK-NEXT:    add w8, w0, w1
; CHECK-NEXT:    add w8, w8, #32 // =32
; CHECK-NEXT:    sub w0, w8, w2
; CHECK-NEXT:    ret
  %t0 = add i32 %a, %b
  %t1 = add i32 %t0, 32
  %r = sub i32 %t1, %c
  ret i32 %r
}
define i32 @sink_sub_of_const_to_sub2(i32 %a, i32 %b, i32 %c) {
; CHECK-LABEL: sink_sub_of_const_to_sub2:
; CHECK:       // %bb.0:
; CHECK-NEXT:    add w8, w0, w1
; CHECK-NEXT:    mov w9, #32
; CHECK-NEXT:    sub w8, w9, w8
; CHECK-NEXT:    add w0, w2, w8
; CHECK-NEXT:    ret
  %t0 = add i32 %a, %b
  %t1 = sub i32 %t0, 32
  %r = sub i32 %c, %t1
  ret i32 %r
}

define i32 @sink_add_of_const_to_sub2(i32 %a, i32 %b, i32 %c) {
; CHECK-LABEL: sink_add_of_const_to_sub2:
; CHECK:       // %bb.0:
; CHECK-NEXT:    add w8, w0, w1
; CHECK-NEXT:    add w8, w8, #32 // =32
; CHECK-NEXT:    sub w0, w2, w8
; CHECK-NEXT:    ret
  %t0 = add i32 %a, %b
  %t1 = add i32 %t0, 32
  %r = sub i32 %c, %t1
  ret i32 %r
}
define i32 @sink_sub_of_const_to_sub(i32 %a, i32 %b, i32 %c) {
; CHECK-LABEL: sink_sub_of_const_to_sub:
; CHECK:       // %bb.0:
; CHECK-NEXT:    add w8, w0, w1
; CHECK-NEXT:    sub w8, w8, #32 // =32
; CHECK-NEXT:    sub w0, w8, w2
; CHECK-NEXT:    ret
  %t0 = add i32 %a, %b
  %t1 = sub i32 %t0, 32
  %r = sub i32 %t1, %c
  ret i32 %r
}

; Basic vector tests. Here it is easier to see where the constant operand is.

define <4 x i32> @vec_sink_add_of_const_to_add(<4 x i32> %a, <4 x i32> %b, <4 x i32> %c) {
; CHECK-LABEL: vec_sink_add_of_const_to_add:
; CHECK:       // %bb.0:
; CHECK-NEXT:    adrp x8, .LCPI6_0
; CHECK-NEXT:    ldr q3, [x8, :lo12:.LCPI6_0]
; CHECK-NEXT:    add v0.4s, v0.4s, v1.4s
; CHECK-NEXT:    add v0.4s, v0.4s, v2.4s
; CHECK-NEXT:    add v0.4s, v0.4s, v3.4s
; CHECK-NEXT:    ret
  %t0 = add <4 x i32> %a, %b
  %t1 = add <4 x i32> %t0, <i32 31, i32 undef, i32 33, i32 66>
  %r = add <4 x i32> %t1, %c
  ret <4 x i32> %r
}
define <4 x i32> @vec_sink_sub_of_const_to_add(<4 x i32> %a, <4 x i32> %b, <4 x i32> %c) {
; CHECK-LABEL: vec_sink_sub_of_const_to_add:
; CHECK:       // %bb.0:
; CHECK-NEXT:    adrp x8, .LCPI7_0
; CHECK-NEXT:    ldr q3, [x8, :lo12:.LCPI7_0]
; CHECK-NEXT:    add v0.4s, v0.4s, v1.4s
; CHECK-NEXT:    sub v0.4s, v0.4s, v3.4s
; CHECK-NEXT:    add v0.4s, v0.4s, v2.4s
; CHECK-NEXT:    ret
  %t0 = add <4 x i32> %a, %b
  %t1 = sub <4 x i32> %t0, <i32 12, i32 undef, i32 44, i32 32>
  %r = add <4 x i32> %t1, %c
  ret <4 x i32> %r
}

define <4 x i32> @vec_sink_add_of_const_to_sub(<4 x i32> %a, <4 x i32> %b, <4 x i32> %c) {
; CHECK-LABEL: vec_sink_add_of_const_to_sub:
; CHECK:       // %bb.0:
; CHECK-NEXT:    adrp x8, .LCPI8_0
; CHECK-NEXT:    ldr q3, [x8, :lo12:.LCPI8_0]
; CHECK-NEXT:    add v0.4s, v0.4s, v1.4s
; CHECK-NEXT:    add v0.4s, v0.4s, v3.4s
; CHECK-NEXT:    sub v0.4s, v0.4s, v2.4s
; CHECK-NEXT:    ret
  %t0 = add <4 x i32> %a, %b
  %t1 = add <4 x i32> %t0, <i32 86, i32 undef, i32 65, i32 47>
  %r = sub <4 x i32> %t1, %c
  ret <4 x i32> %r
}
define <4 x i32> @vec_sink_sub_of_const_to_sub2(<4 x i32> %a, <4 x i32> %b, <4 x i32> %c) {
; ALL-LABEL: vec_sink_sub_of_const_to_sub2:
; ALL:       # %bb.0:
; ALL-NEXT:    paddd %xmm1, %xmm0
; ALL-NEXT:    movdqa {{.*#+}} xmm1 = <93,u,45,81>
; ALL-NEXT:    psubd %xmm0, %xmm1
; ALL-NEXT:    paddd %xmm2, %xmm1
; ALL-NEXT:    movdqa %xmm1, %xmm0
; ALL-NEXT:    ret{{[l|q]}}
; CHECK-LABEL: vec_sink_sub_of_const_to_sub2:
; CHECK:       // %bb.0:
; CHECK-NEXT:    adrp x8, .LCPI9_0
; CHECK-NEXT:    ldr q3, [x8, :lo12:.LCPI9_0]
; CHECK-NEXT:    add v0.4s, v0.4s, v1.4s
; CHECK-NEXT:    sub v0.4s, v3.4s, v0.4s
; CHECK-NEXT:    add v0.4s, v2.4s, v0.4s
; CHECK-NEXT:    ret
  %t0 = add <4 x i32> %a, %b
  %t1 = sub <4 x i32> %t0, <i32 93, i32 undef, i32 45, i32 81>
  %r = sub <4 x i32> %c, %t1
  ret <4 x i32> %r
}

define <4 x i32> @vec_sink_add_of_const_to_sub2(<4 x i32> %a, <4 x i32> %b, <4 x i32> %c) {
; CHECK-LABEL: vec_sink_add_of_const_to_sub2:
; CHECK:       // %bb.0:
; CHECK-NEXT:    adrp x8, .LCPI10_0
; CHECK-NEXT:    ldr q3, [x8, :lo12:.LCPI10_0]
; CHECK-NEXT:    add v0.4s, v0.4s, v1.4s
; CHECK-NEXT:    add v0.4s, v0.4s, v3.4s
; CHECK-NEXT:    sub v0.4s, v2.4s, v0.4s
; CHECK-NEXT:    ret
  %t0 = add <4 x i32> %a, %b
  %t1 = add <4 x i32> %t0, <i32 51, i32 undef, i32 61, i32 92>
  %r = sub <4 x i32> %c, %t1
  ret <4 x i32> %r
}
define <4 x i32> @vec_sink_sub_of_const_to_sub(<4 x i32> %a, <4 x i32> %b, <4 x i32> %c) {
; CHECK-LABEL: vec_sink_sub_of_const_to_sub:
; CHECK:       // %bb.0:
; CHECK-NEXT:    adrp x8, .LCPI11_0
; CHECK-NEXT:    ldr q3, [x8, :lo12:.LCPI11_0]
; CHECK-NEXT:    add v0.4s, v0.4s, v1.4s
; CHECK-NEXT:    sub v0.4s, v0.4s, v3.4s
; CHECK-NEXT:    sub v0.4s, v0.4s, v2.4s
; CHECK-NEXT:    ret
  %t0 = add <4 x i32> %a, %b
  %t1 = sub <4 x i32> %t0, <i32 49, i32 undef, i32 45, i32 21>
  %r = sub <4 x i32> %t1, %c
  ret <4 x i32> %r
}

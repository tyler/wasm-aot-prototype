;; RUN: sexpr-dump %s > %t1 | %sexpr-wasm %t1 > %t2
;; RUN: sexpr-dump %t1 > %t3
;; RUN: diff %t1 %t3
(module (func (local i32) (local $n i64)))

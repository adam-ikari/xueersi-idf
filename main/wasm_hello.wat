;; wasm_hello.wat — zero-import wasm module for WAMR engine verification
;; Build: wat2wasm wasm_hello.wat -o build/esp-idf/main/wasm_hello.wasm

(module
  (func (export "test") (result i32)
    i32.const 42
  )
)
;; wasm_game.wat — Xiaomiao 3D game demo for TinyGL pipeline
;;
;; Invokes native Host APIs:
;;   (import "env" "render_submit_cube" (func $render_submit_cube (param f32 f32 f32 f32 f32 f32 i32)))
;;   (import "env" "physics_step"       (func $physics_step (param f32)))
;;   (import "env" "physics_spawn"      (func $physics_spawn (param f32 f32 f32 f32) (result i32)))
;;   (import "env" "physics_body_count" (func $physics_body_count (result i32)))
;;   (import "env" "physics_body_pos"   (func $physics_body_pos (param i32 i32 i32 i32)))
;;
;; Build: wat2wasm wasm_game.wat -o wasm_game.wasm

(module
  ;; ── Memory (needed for physics_body_pos ptr args) ──
  (memory (export "memory") 1)

  ;; ── Imports from host ──────────────────────────────────
  (import "env" "render_submit_cube" (func $render_submit_cube
    (param f32 f32 f32 f32 f32 f32 i32)))
  (import "env" "physics_step"       (func $physics_step (param f32)))
  (import "env" "physics_spawn"      (func $physics_spawn
    (param f32 f32 f32 f32) (result i32)))
  (import "env" "physics_body_count" (func $physics_body_count (result i32)))
  (import "env" "physics_body_pos"   (func $physics_body_pos
    (param i32 i32 i32 i32)))

  ;; ── Global state ──────────────────────────────────────
  (global $angle     (mut f32) (f32.const 0.0))
  (global $spawned   (mut i32) (i32.const 0))

  ;; ── game_init() ───────────────────────────────────────
  (func (export "game_init")
    ;; physics_spawn(0, 3, 0, 0.8)
    (call $physics_spawn (f32.const 0.0) (f32.const 3.0) (f32.const 0.0) (f32.const 0.8))
    (drop)
    (global.set $spawned (i32.const 1))
  )

  ;; ── game_update() ─────────────────────────────────────
  (func (export "game_update")
    (local $x f32) (local $y f32) (local $z f32)
    (local $count i32)

    ;; angle += 2.0
    (global.set $angle
      (f32.add (global.get $angle) (f32.const 2.0)))
    ;; wrap angle
    (if (f32.ge (global.get $angle) (f32.const 360.0))
      (then (global.set $angle
        (f32.sub (global.get $angle) (f32.const 360.0)))))

    ;; physics_step(0.033)
    (call $physics_step (f32.const 0.033))

    ;; if physics_body_count() == 0 → respawn
    (local.set $count (call $physics_body_count))
    (if (i32.eqz (local.get $count))
      (then
        (call $physics_spawn (f32.const 0.0) (f32.const 3.0) (f32.const 0.0) (f32.const 0.8))
        (drop)
      ))

    ;; Read body 0 position into locals
    ;; Store x,y,z at offsets 0,4,8 in linear memory
    (i32.store (i32.const 0) (i32.reinterpret_f32 (f32.const 0.0)))
    (i32.store (i32.const 4) (i32.reinterpret_f32 (f32.const 0.0)))
    (i32.store (i32.const 8) (i32.reinterpret_f32 (f32.const 0.0)))

    (call $physics_body_pos
      (i32.const 0)    ;; body index
      (i32.const 0)    ;; &x
      (i32.const 4)    ;; &y
      (i32.const 8))   ;; &z

    (local.set $x (f32.load (i32.const 0)))
    (local.set $y (f32.load (i32.const 4)))
    (local.set $z (f32.load (i32.const 8)))

    ;; render_submit_cube(x, y, z, 1.6, 0.0, angle, 1)  -- ceramic texture
    (call $render_submit_cube
      (local.get $x)
      (local.get $y)
      (local.get $z)
      (f32.const 1.6)      ;; size
      (f32.const 0.0)      ;; rx
      (global.get $angle)  ;; ry
      (i32.const 1))       ;; tex_id = ceramic

    ;; render_submit_cube(0, -2, 0, 1.0, angle*0.5, angle*0.3, 2)  -- checker
    (call $render_submit_cube
      (f32.const 0.0)
      (f32.const -2.0)
      (f32.const 0.0)
      (f32.const 1.0)
      (f32.mul (global.get $angle) (f32.const 0.5))
      (f32.mul (global.get $angle) (f32.const 0.3))
      (i32.const 2))       ;; tex_id = checker
  )
)
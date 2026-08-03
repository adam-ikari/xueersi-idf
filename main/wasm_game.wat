;; wasm_game.wat — Xiaomiao 3D game demo, submitted entirely through GL APIs.
;;
;; The wasm module (core 0) is the sole author of every rendered scene: it calls
;; GL lookalikes (glBegin / glVertex3f / ...), which the host encodes into a
;; command stream that core 1 replays against TinyGL. Textures are referenced
;; by integer NAME only (glBindTexture) — no texture data crosses the API.
;;
;; Build: wat2wasm wasm_game.wat -o wasm_game.wasm

(module
  ;; ── Imports from host (all GL command-stream encoders) ──
  (import "env" "glBegin"          (func $glBegin   (param i32)))
  (import "env" "glEnd"            (func $glEnd))
  (import "env" "glVertex3f"       (func $glVertex3f (param f32 f32 f32)))
  (import "env" "glColor3f"        (func $glColor3f  (param f32 f32 f32)))
  (import "env" "glNormal3f"       (func $glNormal3f (param f32 f32 f32)))
  (import "env" "glTexCoord2f"     (func $glTexCoord2f (param f32 f32)))
  (import "env" "glMatrixMode"     (func $glMatrixMode (param i32)))
  (import "env" "glLoadIdentity"   (func $glLoadIdentity))
  (import "env" "glPushMatrix"     (func $glPushMatrix))
  (import "env" "glPopMatrix"      (func $glPopMatrix))
  (import "env" "glRotatef"        (func $glRotatef (param f32 f32 f32 f32)))
  (import "env" "glTranslatef"     (func $glTranslatef (param f32 f32 f32)))
  (import "env" "glBindTexture"    (func $glBindTexture (param i32 i32)))
  (import "env" "glActiveTexture"  (func $glActiveTexture (param i32)))
  (import "env" "glTexEnvi"        (func $glTexEnvi (param i32 i32 i32)))
  (import "env" "glTexEnvfv"       (func $glTexEnvfv (param i32 i32 f32 f32 f32 f32)))
  (import "env" "glTexOffset"      (func $glTexOffset (param i32 f32 f32)))
  (import "env" "glEnable"         (func $glEnable (param i32)))
  (import "env" "glDisable"        (func $glDisable (param i32)))
  (import "env" "glDepthMask"      (func $glDepthMask (param i32)))
  (import "env" "glClear"          (func $glClear (param i32)))
  (import "env" "glFlush"          (func $glFlush))

  ;; ── GL constants ───────────────────────────────────────
  ;; Begin/end
  (global $GL_QUADS            i32 (i32.const 0x0007))
  ;; Enables
  (global $GL_DEPTH_TEST       i32 (i32.const 0x0B71))
  (global $GL_CULL_FACE        i32 (i32.const 0x0B44))
  (global $GL_LIGHTING         i32 (i32.const 0x0B50))
  (global $GL_LIGHT0           i32 (i32.const 0x4000))
  ;; Matrix
  (global $GL_MODELVIEW        i32 (i32.const 0x1700))
  ;; Texture units + env
  (global $GL_TEXTURE0         i32 (i32.const 0x84C0))
  (global $GL_TEXTURE1         i32 (i32.const 0x84C1))
  (global $GL_TEXTURE2         i32 (i32.const 0x84C2))
  (global $GL_TEXTURE_ENV      i32 (i32.const 0x2300))
  (global $GL_TEXTURE_ENV_MODE i32 (i32.const 0x2200))
  (global $GL_TEXTURE_ENV_COLOR i32 (i32.const 0x2201))
  (global $GL_TEXTURE_2D       i32 (i32.const 0x0DE1))
  (global $GL_ADD              i32 (i32.const 0x0104))
  (global $GL_REPLACE          i32 (i32.const 0x1E01))
  ;; Clear bits
  (global $GL_COLOR_BUFFER_BIT i32 (i32.const 0x00004000))
  (global $GL_DEPTH_BUFFER_BIT i32 (i32.const 0x00000100))
  ;; Texture names (firmware-integrated; integer names, no data in the API)
  (global $TEX_METAL     i32 (i32.const 8))
  (global $TEX_SPECULAR  i32 (i32.const 9))
  (global $TEX_REFLECT   i32 (i32.const 10))
  (global $TEX_CHECKER   i32 (i32.const 2))

  ;; ── Global state ───────────────────────────────────────
  (global $angle    (mut f32) (f32.const 0.0))
  (global $scroll_u (mut f32) (f32.const 0.0))
  (global $scroll_v (mut f32) (f32.const 0.0))

  ;; ── Draw a cube of half-size hs (centred at origin) ────
  (func $cube (param $hs f32)
    (local $h f32)
    (local.set $h (local.get $hs))

    (call $glBegin (global.get $GL_QUADS))

    ;; Front +Z
    (call $glNormal3f (f32.const 0) (f32.const 0) (f32.const 1))
    (call $glTexCoord2f (f32.const 0) (f32.const 0)) (call $glVertex3f (f32.neg (local.get $h)) (f32.neg (local.get $h)) (local.get $h))
    (call $glTexCoord2f (f32.const 1) (f32.const 0)) (call $glVertex3f (local.get $h) (f32.neg (local.get $h)) (local.get $h))
    (call $glTexCoord2f (f32.const 1) (f32.const 1)) (call $glVertex3f (local.get $h) (local.get $h) (local.get $h))
    (call $glTexCoord2f (f32.const 0) (f32.const 1)) (call $glVertex3f (f32.neg (local.get $h)) (local.get $h) (local.get $h))
    ;; Back -Z
    (call $glNormal3f (f32.const 0) (f32.const 0) (f32.const -1))
    (call $glTexCoord2f (f32.const 0) (f32.const 0)) (call $glVertex3f (local.get $h) (f32.neg (local.get $h)) (f32.neg (local.get $h)))
    (call $glTexCoord2f (f32.const 1) (f32.const 0)) (call $glVertex3f (f32.neg (local.get $h)) (f32.neg (local.get $h)) (f32.neg (local.get $h)))
    (call $glTexCoord2f (f32.const 1) (f32.const 1)) (call $glVertex3f (f32.neg (local.get $h)) (local.get $h) (f32.neg (local.get $h)))
    (call $glTexCoord2f (f32.const 0) (f32.const 1)) (call $glVertex3f (local.get $h) (local.get $h) (f32.neg (local.get $h)))
    ;; Top +Y
    (call $glNormal3f (f32.const 0) (f32.const 1) (f32.const 0))
    (call $glTexCoord2f (f32.const 0) (f32.const 0)) (call $glVertex3f (f32.neg (local.get $h)) (local.get $h) (local.get $h))
    (call $glTexCoord2f (f32.const 1) (f32.const 0)) (call $glVertex3f (local.get $h) (local.get $h) (local.get $h))
    (call $glTexCoord2f (f32.const 1) (f32.const 1)) (call $glVertex3f (local.get $h) (local.get $h) (f32.neg (local.get $h)))
    (call $glTexCoord2f (f32.const 0) (f32.const 1)) (call $glVertex3f (f32.neg (local.get $h)) (local.get $h) (f32.neg (local.get $h)))
    ;; Bottom -Y
    (call $glNormal3f (f32.const 0) (f32.const -1) (f32.const 0))
    (call $glTexCoord2f (f32.const 0) (f32.const 0)) (call $glVertex3f (f32.neg (local.get $h)) (f32.neg (local.get $h)) (f32.neg (local.get $h)))
    (call $glTexCoord2f (f32.const 1) (f32.const 0)) (call $glVertex3f (local.get $h) (f32.neg (local.get $h)) (f32.neg (local.get $h)))
    (call $glTexCoord2f (f32.const 1) (f32.const 1)) (call $glVertex3f (local.get $h) (f32.neg (local.get $h)) (local.get $h))
    (call $glTexCoord2f (f32.const 0) (f32.const 1)) (call $glVertex3f (f32.neg (local.get $h)) (f32.neg (local.get $h)) (local.get $h))
    ;; Right +X
    (call $glNormal3f (f32.const 1) (f32.const 0) (f32.const 0))
    (call $glTexCoord2f (f32.const 0) (f32.const 0)) (call $glVertex3f (local.get $h) (f32.neg (local.get $h)) (local.get $h))
    (call $glTexCoord2f (f32.const 1) (f32.const 0)) (call $glVertex3f (local.get $h) (f32.neg (local.get $h)) (f32.neg (local.get $h)))
    (call $glTexCoord2f (f32.const 1) (f32.const 1)) (call $glVertex3f (local.get $h) (local.get $h) (f32.neg (local.get $h)))
    (call $glTexCoord2f (f32.const 0) (f32.const 1)) (call $glVertex3f (local.get $h) (local.get $h) (local.get $h))
    ;; Left -X
    (call $glNormal3f (f32.const -1) (f32.const 0) (f32.const 0))
    (call $glTexCoord2f (f32.const 0) (f32.const 0)) (call $glVertex3f (f32.neg (local.get $h)) (f32.neg (local.get $h)) (f32.neg (local.get $h)))
    (call $glTexCoord2f (f32.const 1) (f32.const 0)) (call $glVertex3f (f32.neg (local.get $h)) (f32.neg (local.get $h)) (local.get $h))
    (call $glTexCoord2f (f32.const 1) (f32.const 1)) (call $glVertex3f (f32.neg (local.get $h)) (local.get $h) (local.get $h))
    (call $glTexCoord2f (f32.const 0) (f32.const 1)) (call $glVertex3f (f32.neg (local.get $h)) (local.get $h) (f32.neg (local.get $h)))

    (call $glEnd)
  )

  ;; ── game_init: set up state (textures already uploaded by gl_init) ──
  (func (export "game_init")
    (call $glEnable (global.get $GL_DEPTH_TEST))
    (call $glEnable (global.get $GL_CULL_FACE))
    (call $glEnable (global.get $GL_LIGHTING))
    (call $glEnable (global.get $GL_LIGHT0))
    (call $glMatrixMode (global.get $GL_MODELVIEW))
    (call $glLoadIdentity)
    (call $glFlush)
  )

  ;; ── game_update: submit the full scene through GL APIs ──
  (func (export "game_update")
    ;; animate
    (global.set $angle (f32.add (global.get $angle) (f32.const 2.0)))
    (if (f32.ge (global.get $angle) (f32.const 360.0))
      (then (global.set $angle (f32.sub (global.get $angle) (f32.const 360.0)))))
    (global.set $scroll_u (f32.add (global.get $scroll_u) (f32.const 0.002)))
    (global.set $scroll_v (f32.add (global.get $scroll_v) (f32.const 0.001)))

    (call $glClear (i32.or (global.get $GL_COLOR_BUFFER_BIT) (global.get $GL_DEPTH_BUFFER_BIT)))

    ;; camera
    (call $glMatrixMode (global.get $GL_MODELVIEW))
    (call $glLoadIdentity)
    (call $glTranslatef (f32.const 0) (f32.const 0) (f32.const -3.5))
    (call $glRotatef (f32.const 25) (f32.const 1) (f32.const 0) (f32.const 0))
    (call $glRotatef (global.get $angle) (f32.const 0) (f32.const 1) (f32.const 0))

    ;; ── Metal cube: 3-layer additive multi-texture ──
    ;; unit 0 = metal base (REPLACE)
    (call $glActiveTexture (global.get $GL_TEXTURE0))
    (call $glBindTexture (global.get $GL_TEXTURE_2D) (global.get $TEX_METAL))
    (call $glTexEnvi (global.get $GL_TEXTURE_ENV) (global.get $GL_TEXTURE_ENV_MODE) (global.get $GL_REPLACE))
    ;; unit 1 = desert reflection (ADD, weight 0.5, scrolling)
    (call $glActiveTexture (global.get $GL_TEXTURE1))
    (call $glBindTexture (global.get $GL_TEXTURE_2D) (global.get $TEX_REFLECT))
    (call $glTexEnvi (global.get $GL_TEXTURE_ENV) (global.get $GL_TEXTURE_ENV_MODE) (global.get $GL_ADD))
    (call $glTexEnvfv (global.get $GL_TEXTURE_ENV) (global.get $GL_TEXTURE_ENV_COLOR)
      (f32.const 0.5) (f32.const 0.5) (f32.const 0.5) (f32.const 1.0))
    (call $glTexOffset (global.get $GL_TEXTURE1) (global.get $scroll_u) (global.get $scroll_v))
    ;; unit 2 = specular highlight (ADD, weight 0.35)
    (call $glActiveTexture (global.get $GL_TEXTURE2))
    (call $glBindTexture (global.get $GL_TEXTURE_2D) (global.get $TEX_SPECULAR))
    (call $glTexEnvi (global.get $GL_TEXTURE_ENV) (global.get $GL_TEXTURE_ENV_MODE) (global.get $GL_ADD))
    (call $glTexEnvfv (global.get $GL_TEXTURE_ENV) (global.get $GL_TEXTURE_ENV_COLOR)
      (f32.const 0.35) (f32.const 0.35) (f32.const 0.35) (f32.const 1.0))
    (call $glActiveTexture (global.get $GL_TEXTURE0))

    ;; draw metal cube (size 1.6, rotating)
    (call $glPushMatrix)
    (call $glRotatef (global.get $angle) (f32.const 0) (f32.const 1) (f32.const 0))
    (call $cube (f32.const 0.8))
    (call $glPopMatrix)

    ;; reset units 1+ to REPLACE so the small cube isn't multi-textured
    (call $glActiveTexture (global.get $GL_TEXTURE1))
    (call $glTexEnvi (global.get $GL_TEXTURE_ENV) (global.get $GL_TEXTURE_ENV_MODE) (global.get $GL_REPLACE))
    (call $glActiveTexture (global.get $GL_TEXTURE2))
    (call $glTexEnvi (global.get $GL_TEXTURE_ENV) (global.get $GL_TEXTURE_ENV_MODE) (global.get $GL_REPLACE))
    (call $glActiveTexture (global.get $GL_TEXTURE0))

    ;; small checker cube below
    (call $glBindTexture (global.get $GL_TEXTURE_2D) (global.get $TEX_CHECKER))
    (call $glPushMatrix)
    (call $glTranslatef (f32.const 0) (f32.const -2) (f32.const 0))
    (call $glRotatef (f32.mul (global.get $angle) (f32.const 0.5)) (f32.const 1) (f32.const 0) (f32.const 0))
    (call $glRotatef (f32.mul (global.get $angle) (f32.const 0.3)) (f32.const 0) (f32.const 1) (f32.const 0))
    (call $cube (f32.const 0.5))
    (call $glPopMatrix)

    ;; publish the frame (core 1 replays it)
    (call $glFlush)
  )
)

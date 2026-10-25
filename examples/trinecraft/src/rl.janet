# Trinecraft — raylib 3D binding surface.
#
# This is the ONLY module permitted to touch ffi/struct or ffi/defbind-alias.
# Everything above it sees plain Janet values: tuples for vectors and colors,
# tables for meshes. Follows the pattern proven in ttfx/janet-port/src/gui.janet:
# all raylib calls go through bender's built-in FFI against libraylib directly,
# with no C shim written anywhere. Struct-by-value params marshal as tuples;
# the lazy context resolves symbols at first call, which keeps this module
# compilable as a single thunk (mkimage/AOT) and runnable interpreted.
#
# Verified against raylib 6.2.

(def raylib-candidates
  ["/opt/homebrew/lib/libraylib.dylib"
   "/usr/local/lib/libraylib.dylib"
   "/usr/lib/libraylib.so"
   "libraylib.dylib"])

(defn find-raylib []
  (def env (os/getenv "TRINECRAFT_RAYLIB"))
  (if (and env (not (empty? env)))
    env
    (do
      (var found nil)
      (each c raylib-candidates
        (when (and (nil? found) (= :file (os/stat c :mode)))
          (set found c)))
      (or found "libraylib.dylib"))))

(ffi/context (find-raylib) :lazy true)

#
# --- Struct types ------------------------------------------------------
#
# Layouts transcribed from raylib.h 6.2. ffi/struct computes padding, so
# fields are listed in declaration order and offsets are derived, never
# hardcoded.

(def vec2-type (ffi/struct :float :float))
(def vec3-type (ffi/struct :float :float :float))
(def vec4-type (ffi/struct :float :float :float :float))
(def color-type (ffi/struct :uchar :uchar :uchar :uchar))
(def rect-type (ffi/struct :float :float :float :float))

# Matrix: 16 floats, column-listed as m0,m4,m8,m12 / m1,... in the header,
# but memory order is simply 16 contiguous floats.
(def matrix-type (ffi/struct :float :float :float :float
                             :float :float :float :float
                             :float :float :float :float
                             :float :float :float :float))

# Texture2D { uint id; int width; int height; int mipmaps; int format; }
(def texture-type (ffi/struct :uint :int :int :int :int))
# Image { void *data; int width; int height; int mipmaps; int format; }
(def image-type (ffi/struct :ptr :int :int :int :int))
# Shader { uint id; int *locs; }
(def shader-type (ffi/struct :uint :ptr))
# MaterialMap { Texture2D texture; Color color; float value; }
(def material-map-type (ffi/struct texture-type color-type :float))
# Material { Shader shader; MaterialMap *maps; float params[4]; }
(def material-type (ffi/struct shader-type :ptr :float :float :float :float))
# Camera3D { Vector3 position, target, up; float fovy; int projection; }
(def camera3d-type (ffi/struct vec3-type vec3-type vec3-type :float :int))
# Ray { Vector3 position; Vector3 direction; }
(def ray-type (ffi/struct vec3-type vec3-type))
# BoundingBox { Vector3 min; Vector3 max; }
(def bbox-type (ffi/struct vec3-type vec3-type))

# Mesh, raylib 6.2 field order:
#   int vertexCount; int triangleCount;
#   float *vertices, *texcoords, *texcoords2, *normals, *tangents;
#   unsigned char *colors; unsigned short *indices;
#   int boneCount; unsigned char *boneIndices; float *boneWeights;
#   float *animVertices, *animNormals;
#   unsigned int vaoId; unsigned int *vboId;
(def mesh-type (ffi/struct :int :int
                           :ptr :ptr :ptr :ptr :ptr :ptr :ptr
                           :int :ptr :ptr :ptr :ptr
                           :uint :ptr))

# Field offsets are derived from the struct info, never assumed, so a raylib
# layout change surfaces as a wrong-looking offset rather than memory corruption.
(def mesh-offsets
  (map |($ :offset) ((ffi/struct-info mesh-type) :fields)))

(def mesh-field
  "Symbolic index into mesh-offsets, by raylib field name."
  {:vertex-count 0 :triangle-count 1
   :vertices 2 :texcoords 3 :texcoords2 4 :normals 5 :tangents 6
   :colors 7 :indices 8
   :bone-count 9 :bone-indices 10 :bone-weights 11
   :anim-vertices 12 :anim-normals 13
   :vao-id 14 :vbo-id 15})

(defn mesh-offset
  "Byte offset of a Mesh field, by keyword."
  [field]
  (get mesh-offsets (get mesh-field field)))

#
# --- Window, frame, timing --------------------------------------------
#

(ffi/defbind-alias InitWindow init-window :void [w :int h :int title :string])
(ffi/defbind-alias CloseWindow close-window :void [])
(ffi/defbind-alias WindowShouldClose window-should-close :bool [])
(ffi/defbind-alias IsWindowReady window-ready? :bool [])
(ffi/defbind-alias SetConfigFlags set-config-flags :void [flags :int])
(ffi/defbind-alias SetTargetFPS set-target-fps :void [fps :int])
(ffi/defbind-alias GetFPS get-fps :int [])
(ffi/defbind-alias GetFrameTime get-frame-time :float [])
(ffi/defbind-alias GetTime get-time :double [])
(ffi/defbind-alias GetScreenWidth screen-width :int [])
(ffi/defbind-alias GetScreenHeight screen-height :int [])
(ffi/defbind-alias SetWindowTitle set-window-title :void [t :string])
(ffi/defbind-alias TakeScreenshot take-screenshot :void [name :string])

(ffi/defbind-alias BeginDrawing begin-drawing :void [])
(ffi/defbind-alias EndDrawing end-drawing :void [])
(ffi/defbind-alias ClearBackground clear-background :void [c color-type])
(ffi/defbind-alias BeginMode3D begin-mode-3d :void [cam camera3d-type])
(ffi/defbind-alias EndMode3D end-mode-3d :void [])

#
# --- Input -------------------------------------------------------------
#

(ffi/defbind-alias IsKeyPressed key-pressed? :bool [key :int])
(ffi/defbind-alias IsKeyDown key-down? :bool [key :int])
(ffi/defbind-alias IsKeyReleased key-released? :bool [key :int])
(ffi/defbind-alias IsMouseButtonPressed mouse-pressed? :bool [b :int])
(ffi/defbind-alias IsMouseButtonDown mouse-down? :bool [b :int])
(ffi/defbind-alias GetMousePosition mouse-position vec2-type [])
(ffi/defbind-alias GetMouseDelta mouse-delta vec2-type [])
(ffi/defbind-alias GetMouseWheelMove mouse-wheel :float [])
(ffi/defbind-alias DisableCursor disable-cursor :void [])
(ffi/defbind-alias EnableCursor enable-cursor :void [])
(ffi/defbind-alias IsCursorHidden cursor-hidden? :bool [])

#
# --- Textures and images ----------------------------------------------
#

(ffi/defbind-alias LoadImage load-image image-type [path :string])
(ffi/defbind-alias GenImageColor gen-image-color image-type
  [w :int h :int c color-type])
(ffi/defbind-alias ImageDrawRectangle image-draw-rectangle :void
  [img :ptr x :int y :int w :int h :int c color-type])
(ffi/defbind-alias UnloadImage unload-image :void [img image-type])
(ffi/defbind-alias LoadTextureFromImage load-texture-from-image texture-type
  [img image-type])
(ffi/defbind-alias LoadTexture load-texture texture-type [path :string])
(ffi/defbind-alias UnloadTexture unload-texture :void [t texture-type])
(ffi/defbind-alias SetTextureFilter set-texture-filter :void
  [t texture-type filter :int])
(ffi/defbind-alias SetTextureWrap set-texture-wrap :void
  [t texture-type wrap :int])
(ffi/defbind-alias GenTextureMipmaps gen-texture-mipmaps :void [t :ptr])

#
# --- Materials and shaders --------------------------------------------
#

(ffi/defbind-alias LoadMaterialDefault load-material-default material-type [])
(ffi/defbind-alias LoadShader load-shader shader-type [vs :string fs :string])
(ffi/defbind-alias UnloadShader unload-shader :void [s shader-type])
(ffi/defbind-alias GetShaderLocation shader-location :int
  [s shader-type name :string])
(ffi/defbind-alias SetShaderValue set-shader-value :void
  [s shader-type loc :int value :ptr type :int])

#
# --- Mesh upload and draw ---------------------------------------------
#

(ffi/defbind-alias UploadMesh upload-mesh-raw :void [m :ptr dynamic :bool])
(ffi/defbind-alias UpdateMeshBuffer update-mesh-buffer :void
  [m mesh-type index :int data :ptr size :int offset :int])
(ffi/defbind-alias UnloadMesh unload-mesh-raw :void [m mesh-type])
(ffi/defbind-alias DrawMesh draw-mesh :void
  [m mesh-type mat material-type xform matrix-type])
(ffi/defbind-alias DrawMeshInstanced draw-mesh-instanced :void
  [m mesh-type mat material-type transforms :ptr count :int])

#
# --- Debug / immediate draw -------------------------------------------
#

(ffi/defbind-alias DrawGrid draw-grid :void [slices :int spacing :float])
(ffi/defbind-alias DrawCubeWires draw-cube-wires :void
  [pos vec3-type w :float h :float l :float c color-type])
(ffi/defbind-alias DrawLine3D draw-line-3d :void
  [a vec3-type b vec3-type c color-type])
(ffi/defbind-alias DrawFPS draw-fps :void [x :int y :int])
(ffi/defbind-alias DrawText draw-text :void
  [t :string x :int y :int size :int c color-type])
(ffi/defbind-alias DrawRectangle draw-rectangle :void
  [x :int y :int w :int h :int c color-type])

#
# --- Constants ---------------------------------------------------------
#

(def key-escape 256) (def key-space 32) (def key-left-shift 340)
(def key-w 87) (def key-a 65) (def key-s 83) (def key-d 68)
(def key-q 81) (def key-e 69) (def key-f 70) (def key-tab 258)
(def key-one 49) (def key-two 50) (def key-three 51) (def key-four 52)

(def mouse-button-left 0)
(def mouse-button-right 1)

(def flag-vsync-hint 64)
(def flag-window-resizable 4)
(def flag-msaa-4x-hint 32)

(def camera-perspective 0)
(def camera-orthographic 1)

(def texture-filter-point 0)      # nearest-neighbour: mandatory for block atlases
(def texture-filter-bilinear 1)
(def texture-wrap-clamp 1)

(def material-map-albedo 0)

(def white [0xff 0xff 0xff 0xff])
(def black [0x00 0x00 0x00 0xff])
(def sky-blue [0x8f 0xc8 0xf0 0xff])
(def red [0xe0 0x30 0x30 0xff])
(def yellow [0xf0 0xd0 0x20 0xff])

(def identity-matrix [1 0 0 0  0 1 0 0  0 0 1 0  0 0 0 1])

(defn translation-matrix
  "Column-major translation matrix, the layout raylib's DrawMesh expects."
  [x y z]
  [1 0 0 0
   0 1 0 0
   0 0 1 0
   x y z 1])

#
# --- Mesh helpers ------------------------------------------------------
#
# A Trinecraft mesh is a Janet table holding the packed raylib Mesh struct
# alongside references to the Janet buffers backing it. Holding those
# references matters: raylib keeps raw pointers into them, so if the buffers
# were collected the GPU-side pointers would dangle on any later update.

(defn mesh-pack
  ``Pack a raylib Mesh struct into a fresh buffer pointing at caller-owned
  vertex data. `verts` and `norms` are float32 buffers (3 per vertex), `uvs`
  float32 (2 per vertex), `idxs` uint16. Returns a mesh table; the GPU knows
  nothing about it until mesh-upload is called.``
  [vcount tcount verts uvs norms idxs]
  (def buf (buffer/new-filled (ffi/size mesh-type) 0))
  (ffi/write :int vcount buf (mesh-offset :vertex-count))
  (ffi/write :int tcount buf (mesh-offset :triangle-count))
  (ffi/write :ptr verts buf (mesh-offset :vertices))
  (ffi/write :ptr uvs buf (mesh-offset :texcoords))
  (ffi/write :ptr norms buf (mesh-offset :normals))
  (ffi/write :ptr idxs buf (mesh-offset :indices))
  @{:struct buf
    :vertex-count vcount
    :triangle-count tcount
    # keep the backing buffers alive for as long as the mesh is
    :verts verts :uvs uvs :norms norms :idxs idxs
    :uploaded false
    :value nil})

(defn mesh-upload
  ``Hand the packed mesh to the GPU. raylib fills vaoId/vboId in place.
  Returns the mesh, with :value holding the by-value struct that DrawMesh
  takes and :vao-id the id the driver assigned (non-zero on success).``
  [mesh &opt dynamic]
  (default dynamic false)
  (upload-mesh-raw (mesh :struct) dynamic)
  (put mesh :vao-id (ffi/read :uint (mesh :struct) (mesh-offset :vao-id)))
  (put mesh :value (ffi/read mesh-type (mesh :struct)))
  (put mesh :uploaded true)
  mesh)

(defn mesh-uploaded?
  "True when the GPU accepted this mesh (vaoId is non-zero)."
  [mesh]
  (and (mesh :uploaded) (pos? (or (mesh :vao-id) 0))))

(defn mesh-draw
  "Draw an uploaded mesh with a material at a world offset."
  [mesh material &opt xform]
  (when (mesh-uploaded? mesh)
    (draw-mesh (mesh :value) material (or xform identity-matrix))))

(defn mesh-unload
  "Release GPU buffers. Safe to call on a never-uploaded mesh."
  [mesh]
  (when (mesh-uploaded? mesh)
    (unload-mesh-raw (mesh :value))
    (put mesh :uploaded false)
    (put mesh :vao-id 0))
  mesh)

(defn material-set-texture
  ``Point a material's albedo map at a texture. Material.maps is a pointer to
  an array of MaterialMap; the albedo map is index 0 and its Texture2D sits at
  offset 0 within it, so the texture is written straight into maps[0].``
  [material texture]
  (def maps-ptr (get material 1))
  (ffi/write texture-type texture maps-ptr 0)
  material)

(defn camera3d
  "Build a Camera3D tuple: position, target, up, fov in degrees."
  [pos target up fovy]
  [pos target up (* 1.0 fovy) camera-perspective])

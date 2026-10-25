# Headless checks for the raylib binding surface.
# Verifies struct layouts against raylib 6.2's real sizes and that every bound
# symbol resolves in libraylib — without opening a window, so this can run in
# CI. Run: ./build/bender examples/trinecraft/test/rl-test.janet

(import ./../src/rl :as rl)

(var failures 0)
(defn check [name ok &opt detail]
  (if ok
    (printf "  ok   %s" name)
    (do (++ failures)
        (printf "  FAIL %s%s" name (if detail (string " -- " detail) "")))))

(print "struct sizes (raylib 6.2, LP64):")
# Vector2=8 Vector3=12 Color=4 Matrix=64 Texture2D=20 Shader=16
# MaterialMap=20+4+4=28 (texture 20, color 4, value 4)
# Material = Shader(16) + ptr(8) + 4 floats(16) = 40
# Camera3D = 12+12+12+4+4 = 44
# Mesh = 8 + 7*8 + (4 pad 4) + 4*8 + (4 pad 4) + 8 = 120
(def expected
  {"Vector2" [rl/vec2-type 8]
   "Vector3" [rl/vec3-type 12]
   "Color" [rl/color-type 4]
   "Rectangle" [rl/rect-type 16]
   "Matrix" [rl/matrix-type 64]
   "Texture2D" [rl/texture-type 20]
   "Shader" [rl/shader-type 16]
   "MaterialMap" [rl/material-map-type 28]
   "Material" [rl/material-type 40]
   "Camera3D" [rl/camera3d-type 44]
   "Mesh" [rl/mesh-type 120]})

(each name (sort (keys expected))
  (def [t want] (get expected name))
  (def got (ffi/size t))
  (check (string/format "sizeof %s = %d" name want)
         (= got want)
         (string/format "got %d" got)))

(print "\nMesh field offsets:")
# Derived, not hardcoded: assert the ones the mesher and uploader depend on.
(def want-offsets
  {:vertex-count 0 :triangle-count 4 :vertices 8 :texcoords 16
   :normals 32 :indices 56 :vao-id 104 :vbo-id 112})
(each field (sort (keys want-offsets))
  (def want (get want-offsets field))
  (def got (rl/mesh-offset field))
  (check (string/format "%p at %d" field want)
         (= got want)
         (string/format "got %d" got)))

(print "\nsymbol resolution (lazy FFI binds on first reference):")
# ffi/lookup raises if the symbol is absent from the library, which catches a
# typo'd or version-removed raylib function without needing a GL context.
(def ctx (ffi/native (rl/find-raylib)))
(def symbols
  ["InitWindow" "CloseWindow" "WindowShouldClose" "BeginDrawing" "EndDrawing"
   "ClearBackground" "BeginMode3D" "EndMode3D" "SetTargetFPS" "GetFrameTime"
   "GetFPS" "TakeScreenshot" "IsKeyDown" "IsKeyPressed" "IsMouseButtonPressed"
   "GetMouseDelta" "DisableCursor" "EnableCursor" "LoadImage" "GenImageColor"
   "ImageDrawRectangle" "LoadTextureFromImage" "LoadTexture" "UnloadTexture"
   "SetTextureFilter" "LoadMaterialDefault" "LoadShader" "GetShaderLocation"
   "SetShaderValue" "UploadMesh" "UpdateMeshBuffer" "UnloadMesh" "DrawMesh"
   "DrawMeshInstanced" "DrawGrid" "DrawCubeWires" "DrawLine3D" "DrawFPS"
   "DrawText" "DrawRectangle"])
(var missing @[])
(each s symbols
  (if (try (do (ffi/lookup ctx s) true) ([_] false))
    nil
    (array/push missing s)))
(check (string/format "%d raylib symbols resolve" (length symbols))
       (empty? missing)
       (string "missing: " (string/join missing ", ")))

(print "\nmesh packing (no GPU needed):")
(def verts (buffer/new-filled 36 0))   # 3 verts * 3 floats
(def uvs (buffer/new-filled 24 0))     # 3 verts * 2 floats
(def norms (buffer/new-filled 36 0))
(def idxs (buffer/new-filled 6 0))     # 3 indices * uint16
(def m (rl/mesh-pack 3 1 verts uvs norms idxs))
(check "mesh-pack records vertex count" (= 3 (m :vertex-count)))
(check "mesh-pack records triangle count" (= 1 (m :triangle-count)))
(check "struct buffer is Mesh-sized" (= (length (m :struct)) (ffi/size rl/mesh-type)))
(check "vertexCount written into struct"
       (= 3 (ffi/read :int (m :struct) (rl/mesh-offset :vertex-count))))
(check "triangleCount written into struct"
       (= 1 (ffi/read :int (m :struct) (rl/mesh-offset :triangle-count))))
# NOTE: read pointer fields as :ptr, not :uint64. In bender `ffi/read :uint64`
# yields an abstract <core/u64>, which is never `=` to the integer 0, so a
# null check against 0 silently always "passes". `:ptr` yields nil for NULL
# and a :pointer otherwise, which is the honest test.
(check "vertices pointer is non-null"
       (not (nil? (ffi/read :ptr (m :struct) (rl/mesh-offset :vertices)))))
(check "texcoords pointer is non-null"
       (not (nil? (ffi/read :ptr (m :struct) (rl/mesh-offset :texcoords)))))
(check "normals pointer is non-null"
       (not (nil? (ffi/read :ptr (m :struct) (rl/mesh-offset :normals)))))
(check "indices pointer is non-null"
       (not (nil? (ffi/read :ptr (m :struct) (rl/mesh-offset :indices)))))
(check "tangents pointer left null"
       (nil? (ffi/read :ptr (m :struct) (rl/mesh-offset :tangents))))
(check "texcoords2 pointer left null"
       (nil? (ffi/read :ptr (m :struct) (rl/mesh-offset :texcoords2))))
(check "vaoId starts at zero"
       (zero? (ffi/read :uint (m :struct) (rl/mesh-offset :vao-id))))
(check "backing buffers are retained" (and (m :verts) (m :uvs) (m :norms) (m :idxs)))
(check "not uploaded before mesh-upload" (not (rl/mesh-uploaded? m)))

(print "\nmatrix helpers:")
(check "identity is 16 floats" (= 16 (length rl/identity-matrix)))
(def tm (rl/translation-matrix 5 6 7))
(check "translation is 16 floats" (= 16 (length tm)))
(check "translation puts x,y,z in slots 12..14"
       (and (= 5 (get tm 12)) (= 6 (get tm 13)) (= 7 (get tm 14))))

(def cam (rl/camera3d [1 2 3] [0 0 0] [0 1 0] 70))
(check "camera3d has 5 fields" (= 5 (length cam)))
(check "camera3d fov is float" (= 70.0 (get cam 3)))
(check "camera3d projection is perspective" (= rl/camera-perspective (get cam 4)))

(printf "\n%s  (%d failure%s)"
        (if (zero? failures) "ALL CHECKS PASSED" "CHECKS FAILED")
        failures (if (= 1 failures) "" "s"))
(os/exit (if (zero? failures) 0 1))

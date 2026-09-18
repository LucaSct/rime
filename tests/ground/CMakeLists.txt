# m17.8 — the ground's seam: one authored surface, three derived artefacts that agree.
#
# GPU-free by construction. `derive_mesh` returns a CpuMesh (no device), `derive_collider` returns a
# ShapeDesc, and the proof that the drawn edge and the standable edge are the same edge is a
# raycast — so this runs on every CI OS and under both sanitizers, which is where a geometry
# disagreement should be caught.
rime_add_test(ground_test ground_test.cpp)
target_link_libraries(ground_test
    PRIVATE rime::ground rime::physics rime::render rime::ecs rime::core)

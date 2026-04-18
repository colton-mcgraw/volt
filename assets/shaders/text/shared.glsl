// shared.glsl — include in all three shaders

struct CurveInput {
    vec2 p0, p1, p2, p3;  // p3 unused for quadratics
    uint path_id;           // which path this curve belongs to
    uint is_cubic;          // 0 = quadratic, 1 = cubic
};

struct LineSegment {
    vec2 p0, p1;
    uint path_id;
    uint curve_index;       // source curve, for metadata lookup
};

struct CurveMeta {
    uint segment_offset;    // where in segment buffer this curve starts
    uint segment_count;     // how many segments it produced
};
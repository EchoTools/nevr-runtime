# bvh_core: each rule of src/quest/combat/bvh_core.h removed once; bvh_core_test (the pure core) must fail on every one. The rules of bvh.cpp and bvh.h
# are mutants.d/bvh.sh. Run: src/quest/combat/mutants.sh bvh_core. Each line is `runm <name> <file> <sed> <build dir> <test target>`.
runm bvh_parse_empty_tri src/quest/combat/bvh_core.h "s/if (s0 == 0 || s1 == 0) {/if (s1 == 0) {/" quest-host-combat bvh_core_test
runm bvh_parse_empty_node src/quest/combat/bvh_core.h "s/if (s0 == 0 || s1 == 0) {/if (s0 == 0) {/" quest-host-combat bvh_core_test
runm bvh_parse_tri_modulo src/quest/combat/bvh_core.h "s/if (s0 % sizeof(Tri4) || /if (/" quest-host-combat bvh_core_test
runm bvh_parse_node_modulo src/quest/combat/bvh_core.h "s/s1 % sizeof(Node) || //" quest-host-combat bvh_core_test
runm bvh_parse_max_tri src/quest/combat/bvh_core.h "s/kMaxTri4 = 0x3ffffff/kMaxTri4 = 0x7ffffff/" quest-host-combat bvh_core_test
runm bvh_parse_max_node src/quest/combat/bvh_core.h "s/kMaxNodes = 0xfffffff/kMaxNodes = 0x1fffffff/" quest-host-combat bvh_core_test
runm bvh_parse_max_tri_cmp src/quest/combat/bvh_core.h "s/s0 \/ sizeof(Tri4) > kMaxTri4/false/" quest-host-combat bvh_core_test
runm bvh_parse_max_node_cmp src/quest/combat/bvh_core.h "s/s1 \/ sizeof(Node) > kMaxNodes/false/" quest-host-combat bvh_core_test
runm bvh_parse_tri_follow src/quest/combat/bvh_core.h "s/p0 ? p0 : hdr_addr + kHeaderSize;/p0;/" quest-host-combat bvh_core_test
runm bvh_parse_node_follow src/quest/combat/bvh_core.h "s/p1 ? p1 : hdr_addr + kHeaderSize + s0;/p1;/" quest-host-combat bvh_core_test
runm bvh_parse_follow_off src/quest/combat/bvh_core.h "s/hdr_addr + kHeaderSize + s0;/hdr_addr + kHeaderSize;/" quest-host-combat bvh_core_test
runm bvh_parse_wrap_tri src/quest/combat/bvh_core.h "s/tp + s0 < tp || //" quest-host-combat bvh_core_test
runm bvh_parse_wrap_node src/quest/combat/bvh_core.h "s/np + s1 < np || //" quest-host-combat bvh_core_test
runm bvh_parse_nullpage_tri src/quest/combat/bvh_core.h "s/ || tp < 0x10000//" quest-host-combat bvh_core_test
runm bvh_parse_nullpage_nd src/quest/combat/bvh_core.h "s/ || np < 0x10000//" quest-host-combat bvh_core_test
runm bvh_parse_root src/quest/combat/bvh_core.h "s/tree->root = static_cast<int32_t>(root);/tree->root = 0;/" quest-host-combat bvh_core_test
runm bvh_parse_root_off src/quest/combat/bvh_core.h "s/memcpy(\&root, hdr + 0x40, 4);/memcpy(\&root, hdr + 0x44, 4);/" quest-host-combat bvh_core_test
runm bvh_parse_node_ptr_off src/quest/combat/bvh_core.h "s/memcpy(\&p1, hdr + 0x20, 8);/memcpy(\&p1, hdr + 0x18, 8);/" quest-host-combat bvh_core_test
runm bvh_parse_node_sz_off src/quest/combat/bvh_core.h "s/memcpy(\&s1, hdr + 0x28, 8);/memcpy(\&s1, hdr + 0x30, 8);/" quest-host-combat bvh_core_test
runm bvh_parse_tri_sz_off src/quest/combat/bvh_core.h "s/memcpy(\&s0, hdr + 0x08, 8);/memcpy(\&s0, hdr + 0x10, 8);/" quest-host-combat bvh_core_test
runm bvh_header_size src/quest/combat/bvh_core.h "s/kHeaderSize = 0x50;/kHeaderSize = 0x48;/" quest-host-combat bvh_core_test
runm bvh_ray_finite src/quest/combat/bvh_core.h "s/ok = ok \&\& isfinite(org\[k\]) \&\& isfinite(dir\[k\]);//" quest-host-combat bvh_core_test
runm bvh_ray_finite_org src/quest/combat/bvh_core.h "s/ok = ok \&\& isfinite(org\[k\]) \&\& isfinite(dir\[k\]);/ok = ok \&\& isfinite(dir[k]);/" quest-host-combat bvh_core_test
runm bvh_ray_zero_dir src/quest/combat/bvh_core.h "s/ok = ok \&\& len2 > 0.0f \&\& /ok = ok \&\& /" quest-host-combat bvh_core_test
runm bvh_ray_len_finite src/quest/combat/bvh_core.h "s/ \&\& isfinite(len2)//" quest-host-combat bvh_core_test
runm bvh_ray_range_order src/quest/combat/bvh_core.h "s/ \&\& tmax_ >= tmin_;/;/" quest-host-combat bvh_core_test
runm bvh_box_swap src/quest/combat/bvh_core.h "s/    if (a > z) {/    if (false) {/" quest-host-combat bvh_core_test
runm bvh_box_open src/quest/combat/bvh_core.h "s/return t0 <= t1 ? t0 : -1.0f;/return t0 < t1 ? t0 : -1.0f;/" quest-host-combat bvh_core_test
runm bvh_box_never_miss src/quest/combat/bvh_core.h "s/return t0 <= t1 ? t0 : -1.0f;/return t0;/" quest-host-combat bvh_core_test
runm bvh_tri_u_lo src/quest/combat/bvh_core.h "s/if (u < 0.0f) return false;/if (false) return false;/" quest-host-combat bvh_core_test
runm bvh_tri_v_lo src/quest/combat/bvh_core.h "s/if (v < 0.0f || u + v > 1.0f) return false;/if (u + v > 1.0f) return false;/" quest-host-combat bvh_core_test
runm bvh_tri_uv_sum src/quest/combat/bvh_core.h "s/if (v < 0.0f || u + v > 1.0f) return false;/if (v < 0.0f) return false;/" quest-host-combat bvh_core_test
runm bvh_tri_tmin_open src/quest/combat/bvh_core.h "s/return t > r.tmin \&\& t < tmax;/return t >= r.tmin \&\& t < tmax;/" quest-host-combat bvh_core_test
runm bvh_tri_tmax_open src/quest/combat/bvh_core.h "s/return t > r.tmin \&\& t < tmax;/return t > r.tmin \&\& t <= tmax;/" quest-host-combat bvh_core_test
runm bvh_tri_tmin_ignored src/quest/combat/bvh_core.h "s/return t > r.tmin \&\& t < tmax;/return t < tmax;/" quest-host-combat bvh_core_test
runm bvh_tri_v1_sign src/quest/combat/bvh_core.h "s/const float a\[3\] = {-q.e1\[0\]\[l\], -q.e1\[1\]\[l\], -q.e1\[2\]\[l\]};/const float a[3] = {q.e1[0][l], q.e1[1][l], q.e1[2][l]};/" quest-host-combat bvh_core_test
runm bvh_trace_ray_ok src/quest/combat/bvh_core.h "s/if (!r.ok) return false;/if (false) return false;/" quest-host-combat bvh_core_test
runm bvh_trace_budget src/quest/combat/bvh_core.h "s/2ull \* tree.nnodes + 2;/2ull * tree.nnodes + 1000000;/" quest-host-combat bvh_core_test
runm bvh_trace_budget_tight src/quest/combat/bvh_core.h "s/2ull \* tree.nnodes + 2;/tree.nnodes;/" quest-host-combat bvh_core_test
runm bvh_trace_budget_cmp src/quest/combat/bvh_core.h "s/if (++pops > max_pops) return false;/if (++pops >= max_pops) return false;/" quest-host-combat bvh_core_test
runm bvh_trace_stack_size src/quest/combat/bvh_core.h "s/kStackSize = 128;/kStackSize = 64;/" quest-host-combat bvh_core_test
runm bvh_trace_stack_check src/quest/combat/bvh_core.h "s/if (sp + 2 > kStackSize) return false;/if (sp + 2 > kStackSize + 1) return false;/" quest-host-combat bvh_core_test
runm bvh_trace_stack_off src/quest/combat/bvh_core.h "s/if (sp + 2 > kStackSize) return false;/if (sp + 1 > kStackSize) return false;/" quest-host-combat bvh_core_test
runm bvh_trace_node_bound src/quest/combat/bvh_core.h "s/if (ni >= tree.nnodes) return false;/if (ni > tree.nnodes) return false;/" quest-host-combat bvh_core_test
runm bvh_trace_leaf_bound src/quest/combat/bvh_core.h "s/if (static_cast<uint64_t>(li) + cnt > tree.ntris) return false;/if (static_cast<uint64_t>(li) > tree.ntris) return false;/" quest-host-combat bvh_core_test
runm bvh_trace_empty_child src/quest/combat/bvh_core.h "s/n.child\[0\] == INT32_MIN ? -1.0f : /\/\*x\*\/ /" quest-host-combat bvh_core_test
runm bvh_trace_empty_child1 src/quest/combat/bvh_core.h "s/n.child\[1\] == INT32_MIN ? -1.0f : /\/\*x\*\/ /" quest-host-combat bvh_core_test
runm bvh_trace_empty_lane src/quest/combat/bvh_core.h "s/if (q.id0\[l\] == -1) continue;/\/\*x\*\//" quest-host-combat bvh_core_test
runm bvh_trace_leaf_shift src/quest/combat/bvh_core.h "s/\& 0x7fffffffu) >> 5, cnt/\& 0x7fffffffu) >> 4, cnt/" quest-host-combat bvh_core_test
runm bvh_trace_leaf_count src/quest/combat/bvh_core.h "s/cnt = static_cast<uint32_t>(id) \& 31u;/cnt = static_cast<uint32_t>(id) \& 15u;/" quest-host-combat bvh_core_test
runm bvh_trace_inner_shift src/quest/combat/bvh_core.h "s/static_cast<uint32_t>(id) >> 3;/static_cast<uint32_t>(id) >> 2;/" quest-host-combat bvh_core_test
runm bvh_trace_any_stops src/quest/combat/bvh_core.h "s/if (any) goto done;/if (any \&\& false) goto done;/" quest-host-combat bvh_core_test
runm bvh_trace_near_first src/quest/combat/bvh_core.h "s/(e1 < 0 || e0 <= e1)/(e1 < 0 || e0 >= e1)/" quest-host-combat bvh_core_test
runm bvh_trace_best_closer src/quest/combat/bvh_core.h "s/        best = t;/        best = r.tmax;/" quest-host-combat bvh_core_test
runm bvh_trace_tmax src/quest/combat/bvh_core.h "s/float best = r.tmax;/float best = 1e30f;/" quest-host-combat bvh_core_test
runm bvh_trace_root src/quest/combat/bvh_core.h "s/stack\[sp\] = static_cast<uint32_t>(tree.root);/stack[sp] = 0;/" quest-host-combat bvh_core_test
runm bvh_trace_id1 src/quest/combat/bvh_core.h "s/out->id1 = bq->id1\[bl\];/out->id1 = bq->id0[bl];/" quest-host-combat bvh_core_test
runm bvh_trace_id0 src/quest/combat/bvh_core.h "s/out->id0 = bq->id0\[bl\];/out->id0 = bq->id1[bl];/" quest-host-combat bvh_core_test
runm bvh_trace_u src/quest/combat/bvh_core.h "s/out->u = bu;/out->u = bv; (void)bu;/" quest-host-combat bvh_core_test
runm bvh_trace_v src/quest/combat/bvh_core.h "s/out->v = bv;/out->v = bu; (void)bv;/" quest-host-combat bvh_core_test
runm bvh_trace_normal src/quest/combat/bvh_core.h "s/out->n\[k\] = len > 0 ? g\[k\] \/ len : 0.0f;/out->n[k] = len > 0 ? g[k] : 0.0f;/" quest-host-combat bvh_core_test
runm bvh_trace_normal_zero src/quest/combat/bvh_core.h "s/out->n\[k\] = len > 0 ? g\[k\] \/ len : 0.0f;/out->n[k] = g[k] \/ len;/" quest-host-combat bvh_core_test
runm bvh_trace_hit_t src/quest/combat/bvh_core.h "s/out->t = best;/out->t = r.tmax;/" quest-host-combat bvh_core_test
# Not listed, because the rule is redundant by construction and no test can tell it apart: the `det == 0 || !isfinite(det)` guard in
# TriHit (IEEE division already ends in a miss), `u > 1` (u + v > 1 with v >= 0), the NaN-range compare (tmax >= tmin is false for NaN),

# Each of the two prunes alone is redundant (a box behind a known hit is refused by either), so only both removed together change the work; the pop counter shows it.
runm bvh_prune_both src/quest/combat/bvh_core.h "s/float t0 = r.tmin, t1 = tmax;/float t0 = r.tmin, t1 = r.tmax; (void)tmax;/;s/if (entry\[sp\] > best) continue;/if (false \&\& entry[sp] > best) continue;/" quest-host-combat bvh_core_test

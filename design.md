# Perfevent3 Template Sampler Design

## Problem

`perfevent2` groups one configured event into a record with an array sized to
the number of online CPUs. That works for core events, but it is sparse for
uncore PMUs. For example, AMD L3 events are exposed through representative CPUs
for L3 cache instances, and package events such as `power/energy-pkg/` are
exposed through representative CPUs for packages.

The new sampler should model the measurement axis directly instead of storing
uncore data in `values[n_cpu]`.

## Design

`perfevent3` is a new sampler. It does not change `perfevent2`.

Each plugin instance owns one instance template:

- `cpu`: one slot per monitored CPU.
- `cache`: one slot per cache instance, currently keyed by cache level and id.
- `socket`: one slot per physical package/socket.
- `node`: one slot per NUMA node.
- `pmu`: one slot per exact PMU device name.

The LDMS set contains:

- `instances`: metadata records describing slot `i`.
- `counters`: unscaled event records with `values[instance_count]`.
- `scaled_counters`: scaled event records with `values[instance_count]`.

The invariant is:

```text
event.values[i] belongs to instances[i]
```

This lets L3 events use `values[n_l3]`, package events use
`values[n_socket]`, and CPU events use `values[n_cpu_selected]`.

## Compatibility

`perfevent3` requires one compatible resource domain per plugin instance.
Configs that mix incompatible domains, such as CPU core events and L3 uncore
events, should be split into separate sampler instances.

The existing perfdb loading and PMU event resolution model from `perfevent2` is
reused so that vendor PMU events continue to resolve through the generated or
default perfdb.

## Alternatives Considered

- Fix `perfevent2`: lower disruption for users who already adopted it, but it
  would replace an existing public schema.
- Scalar records: one row per opened perf FD is universally correct but less
  convenient for aligned consumers.
- Auto-split mixed configs: useful later, but it hides resource-domain
  boundaries and makes error reporting harder.
- PMU-family wildcards: useful for names like `amd_iommu_*`, but v1 keeps exact
  PMU names to avoid surprising expansion rules.

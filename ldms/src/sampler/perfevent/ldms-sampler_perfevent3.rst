.. _perfevent3:

==========
perfevent3
==========

---------------------------------------------------
LDMS perf event sampler with template-based axes
---------------------------------------------------

:Date: 29 Jun 2026
:Manual section: 7
:Manual group: LDMS sampler

SYNOPSIS
========

``ldmsd`` configuration:

.. parsed-literal::

   ``load`` ``name``\ =\ *PLUG_INST_NAME* ``plugin``\ =\ **perfevent3**

   ``config`` ``name``\ =\ *PLUG_INST_NAME* ``producer``\ =\ *PRODUCER*
          ``instance``\ =\ *INSTANCE* ``conf``\ =\ *PERF_CONF_JSON*
          [``perfdb``\ =\ *PERFDB_JSON*]

   ``start`` ``name``\ =\ *PLUG_INST_NAME* ``interval``\ =\ *INTERVAL*

DESCRIPTION
===========

``perfevent3`` samples Linux perf events using a template axis. A plugin
instance owns exactly one template, and every event record has a ``values``
array aligned with that template's ``instances`` list.

This keeps uncore data compact. For example, an L3 template has one slot per L3
cache instance instead of one slot per online CPU.

SET FORMAT
==========

The set contains base sampler metrics plus these lists:

.. code:: python

   {
     "instances": [
       {
         "index": _U32_,
         "instance_type": _STR_,   # cpu, cache, socket, node, or pmu
         "name": _STR_,
         "pmu": _STR_,
         "representative_cpu": _S32_,
         "socket_id": _S32_,
         "die_id": _S32_,
         "node_id": _S32_,
         "cache_level": _S32_,
         "cache_id": _S32_,
       },
       ...
     ],
     "counters": [
       {
         "name": _STR_,
         "event_spec": _STR_,
         "pmu": _STR_,
         "pid": _S64_,
         "values": _S64_ARRAY_,    # values[i] belongs to instances[i]
       },
       ...
     ],
     "scaled_counters": [
       {
         "name": _STR_,
         "event_spec": _STR_,
         "pmu": _STR_,
         "pid": _S64_,
         "values": _D64_ARRAY_,    # values[i] belongs to instances[i]
       },
       ...
     ],
   }

CONFIGURATION
=============

The JSON configuration contains a required ``template`` object and an
``events`` list:

.. code:: json

   {
     "template": {
       "type": "cpu|cache|socket|node|pmu",
       "cpus": "0-15",
       "level": 3,
       "pmus": [ "amd_iommu_0", "amd_iommu_1" ]
     },
     "events": [
       { "event": "instructions" }
     ],
     "perfdb": "/path/to/perfdb.json"
   }

Template attributes:

- ``type`` is required.
- ``cpus`` is optional for CPU, cache, socket, and node templates. It filters
  the CPUs considered when building the instance list.
- ``level`` is required for ``cache`` templates.
- ``pmus`` is required for ``pmu`` templates and must list exact PMU device
  names.

Each event must be compatible with every instance in the template. Mixed domains
such as CPU core events and L3 uncore events should be configured as separate
``perfevent3`` plugin instances.

EXAMPLES
========

CPU template:

.. code:: json

   {
     "template": { "type": "cpu", "cpus": "0-7" },
     "events": [
       { "event": "instructions" },
       { "event": "cpu-cycles" }
     ]
   }

L3 cache template:

.. code:: json

   {
     "template": { "type": "cache", "level": 3 },
     "events": [
       { "event": "l3_misses" },
       { "event": "l3_cache_accesses" }
     ]
   }

Socket/package template:

.. code:: json

   {
     "template": { "type": "socket" },
     "events": [
       { "event": "power/energy-pkg/" }
     ]
   }

PMU template:

.. code:: json

   {
     "template": {
       "type": "pmu",
       "pmus": [ "amd_iommu_0", "amd_iommu_1" ]
     },
     "events": [
       { "event": "cmd_processed" }
     ]
   }

SEE ALSO
========

``perf_event_open``\ (2),
``ldmsd``\ (7),
``ldms_sampler_base``\ (7),
``perfevent2``\ (7)

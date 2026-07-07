.. _linux_perf:

==========
linux_perf
==========

------------------------------------------------------
ldmsd plugin for source-instance Linux perf PMU events
------------------------------------------------------

:Date: 03 Jul 2026
:Manual section: 7
:Manual group: LDMS sampler

SYNOPSIS
========

``ldmsd`` configuration commands:

.. parsed-literal::

   ``load`` ``name``\ =\ *PLUG_INST_NAME* ``plugin``\ =\ **linux_perf**

   ``config`` ``name``\ =\ *PLUG_INST_NAME* ``producer``\ =\ *PRODUCER*
          ``instance``\ =\ *INSTANCE* ``conf``\ =\ *PERF_SOURCE_JSON*

   ``start`` ``name``\ =\ *PLUG_INST_NAME* ``interval``\ =\ *INTERVAL*

DESCRIPTION
===========

``linux_perf`` samples one exact Linux perf PMU selected from
``/sys/bus/event_source/devices``. The sampler creates one LDMS source instance
for each selected PMU binding CPU, rather than creating dense arrays sized by
the total number of online CPUs. This keeps uncore PMU data compact and avoids
unused CPU slots.

The recommended LDMS set instance namespace is
``<producer>/linux_perf/<pmu-or-role>``, for example
``nid200251/linux_perf/amd_l3``.

The sampler records raw perf counts and the kernel timing fields
``time_enabled`` and ``time_running``. It does not publish scaled or
multiplex-corrected counter values. Consumers may compute those values from the
raw fields when needed.

CONFIGURATION FILE
==================

The ``conf`` parameter names a JSON file with a ``source`` object and an
``events`` list.

.. code:: json

   {
     "source": {
       "pmu": "power",
       "cpus": "0,64"
     },
     "events": [
       { "name": "energy-pkg" }
     ]
   }

``source.pmu`` is required and must be an exact PMU name under
``/sys/bus/event_source/devices``. PMU globbing, PMU lists, and automatic PMU
family expansion are not supported.

``source.cpus`` is optional. When present, it is a CPU-list filter applied to
the PMU binding CPUs.

Each event has a required ``name`` field. Without ``raw``, the name must resolve
to ``/sys/bus/event_source/devices/<pmu>/events/<name>``.

An event may instead provide explicit raw PMU format fields:

.. code:: json

   {
     "source": {
       "pmu": "amd_l3"
     },
     "events": [
       {
         "name": "l3_lookup_all_requests",
         "raw": {
           "event": "0x04",
           "umask": "0xff"
         }
       }
     ]
   }

For raw events, ``name`` is the LDMS event label and does not need to exist in
the PMU ``events`` directory. Every raw field must exist in the selected PMU's
``format`` directory.

SET FORMAT
==========

The set includes base sampler metrics and two list metrics:

``instances``
   List of source instance records. Each record has ``name``, ``pmu``,
   ``instance_id``, ``binding_cpu``, ``cpus``, and ``labels`` fields.

``events``
   List of event records. Each record has ``name``, ``pmu``, ``unit``,
   ``scale``, ``counts[]``, ``time_enabled[]``, and ``time_running[]`` fields.

For every event record, array slot ``i`` corresponds to ``instances[i]``.

EXAMPLES
========

LDMS configuration for AMD L3:

.. code:: text

   load name=linux_perf_amd_l3 plugin=linux_perf
   config name=linux_perf_amd_l3 producer=nid200251 \
          instance=nid200251/linux_perf/amd_l3 conf=/etc/ldms/linux_perf/amd_l3.json
   start name=linux_perf_amd_l3 interval=1s offset=0

Package energy:

.. code:: json

   {
     "source": {
       "pmu": "power"
     },
     "events": [
       { "name": "energy-pkg" }
     ]
   }

One IOMMU PMU device:

.. code:: json

   {
     "source": {
       "pmu": "amd_iommu_0"
     },
     "events": [
       { "name": "mem_trans_total" },
       { "name": "cmd_processed" }
     ]
   }

CPU PMU filtered to CPUs 0 through 15:

.. code:: json

   {
     "source": {
       "pmu": "cpu",
       "cpus": "0-15"
     },
     "events": [
       { "name": "instructions" }
     ]
   }

NOTES
=====

The process running ``ldmsd`` must have permission to open perf events. On
Linux systems this is controlled by settings such as
``/proc/sys/kernel/perf_event_paranoid`` and capabilities such as
``CAP_PERFMON``.

---
layout: page
title: fi_mon_sampler(1)
tagline: Libfabric Programmer's Manual
---
{% include JB/setup %}


# NAME

fi_mon_sampler  \- Simple sampler for ofi_hook_monitor provider.


# SYNOPSIS
```
 fi_mon_sampler [OPTIONS] <target>		sample from file(s) at <target>
```

# DESCRIPTION

Extract data from the ofi_hook_monitor provider via communication files.
`<target>` can either be one communication file or a folder of files. Data is
exported based on `-f <format>` and either printed to stdout (only for single
files), or stored per communication file at `-o <outpath>`. The sampler can
watch the communication files for changes via the option `-w <msec>` for
repeated sampling.

The name format of the output files is based on the ofi_hook_monitor provider
and is as follows:
`<ppid>_<pid>_<sequential id>_<job id>_<provider name>`.
`ppid` and `pid` are taken from the perspective of the monitored application.
In a batched environment running SLURM, `job id` is set to the SLURM job ID,
otherwise it is set to 0.


# HOW TO RUN

Launch a libfabric application with `FI_HOOK=monitor` to enable the
ofi_hook_monitor provider.
Adjust the monitor provider settings according to [`fi_hook`(7)](fi_hook.7.html).

Then launch the sampler via `fi_mon_sampler -o <output> <target>`. 
By default, the ofi_hook_monitor provider stores data at
`/dev/shm/ofi/<uid>/<hostname>`.

The sampler will generate output files in the directory specified at `<output>`,
	one for each monitored provider.

# OPTIONS

*-w \<msec\>*
: Watch files for changes, check every \<msec\> milliseconds.

*-f \<format\>*
: Output format. Currently only supports CSV.

*-o \<outpath\>*
: Output file path.

*-s*
: Print output to stdout. Only works if `<target>` is a single file.

*-r \<remote URL\>*
: URL of remote output database. Enables monitoring data pusher. Specified
  database endpoint must accept format specified in `-f`.

*-m \<KiB\>*
: Size of pusher data buffer in KiB.

*-l*
: Enable performance logging of sampler. See Environment Variables for output options.

*-v*
: Enable verbose output.

# USAGE EXAMPLES

Launch a libfabric application and enable the ofi_hook_monitor provider:
```bash
FI_HOOK=monitor fi_pingpong [OPTIONS]
```
Launch another `fi_pingpong` with the respective settings.

Finally, launch the sampler:
```bash
fi_mon_sampler -o $HOME -w 1000 -f csv /dev/shm/ofi/$UID/$HOSTNAME
```

# OUTPUT

Output files will be generated in the folder specified at `-o <output>`.

## Formats

### CSV
In `-f csv` mode, this will contain a CSV file with data for all monitored
libfabric functions. For each function, both the `count` and `sum` counters
are exported, indicated by the column name suffix `_c` and `_s` respectively.
In addition, each function is monitored for each data size bucket.
Refer to [`fi_hook`(7)](fi_hook.7.html) for more details.

Example CSV output, first four columns, first three rows:

```csv
mon_recv_0_64_c,mon_recv_0_64_s,mon_recv_64_512_c,mon_recv_64_512_s
0,0,0,0
22529,0,0,0
113664,0,0,0
```

### Influx
In `-f influx` mode, the Influx Line Protocol is used. The measurement name
is `ofi_hook_monitor`.
Example influx output:

```
ofi_hook_monitor,function=mon_recv,
bucket=0_64,pid=1287116,ppid=1284798,
monitor_id=1,job_id=0 count=32,sum=0 1763393626427182336
```

## Remote Output
The option `-r` can be used to set output to a remote database. This database
must accept the format specified in `-f`.
Many databases support the Influx Line protocol.
Should a database require authentication, then you can use the
`MON_SAMPLER_REMOTE_HEADERS` environment variable (see Environment Variables).

# Environment Variables

*MON_SAMPLER_REMOTE_HEADERS*
: Headers as string for remote output pusher (default: '',
  example: `Authorization: Token <token>`).

*MON_SAMPLER_PERF_LOG_PUSHER_OUTPATH*
: Path to output file for sampler log (default: `./log_sampler.csv`)

*MON_SAMPLER_PERF_LOG_SAMPLER_OUTPATH*
: Path to output file for pusher log (default: `./log_pusher.csv`)

*MON_SAMPLER_SPEED_LIMIT_KBS*
: Maximum upload speed in KB/second. (default: unlimited)

*MON_SAMPLER_PUSHER_MAX_WAIT_S*
: Maximum wait time in seconds for pusher before pushing data to database. (default 5)


# Sampler Performance Logging

The maximum achievable frequency at which the monitoring sampler can sample
depends on a variety of factors, such as num number of monitored libfabric
processes or the amount of processed monitoring data.
If option `-r` is specified, then the sampler will additionally launch the pusher
component, which is a second thread  asynchronously uploading batches of
monitored data to a remote database. In order to evaluate which sampling
frequency is achievable without missing sampling deadlines, and in order to tune
sampler and transmission system parameters, a simple performance log can be
enabled by setting `-b`.

In this mode, the sampler will open and populate two CSV files at
`MON_SAMPLER_PERF_LOG_PUSHER_OUTPATH` and `MON_SAMPLER_PERF_LOG_SAMPLER_OUTPATH`,
which contain information on the performance of the sampler and pusher respectively.
These CSV files have the following format:

Sampler:

- `time`: UNIX epoch timestamp of sample iteration
- `num_files`: number of sampled files
- `sample_time_ms`: sample time in milliseconds
- `serialized_elements`: number of serialized elements
- `serialized_bytes`: number of serialized bytes
- `procs_with_data`: processes with data

Pusher:

- `time`: UNIX epoch timestamp of pusher run
- `buffer_size`: size of transmit buffer in bytes
- `upload_time_ms`: upload time in milliseconds


# SEE ALSO

[`fi_hook`(7)](fi_hook.7.html)

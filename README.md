# spart
A user-oriented partition info command for slurm.

Slurm does not contains a command to show user-oriented partition info. I wrote a command. I hope you will find it useful.

## Usage

The spart command accepts a minimal number of options:

```
$ spart --help
usage:

    spart {options}

  options:

    -h/--help                  display this help
    -f/--format <format>       display the parition summary in the
                               specified format
    -p/--partition <part-list> select specific partitions to include
                               in the summary

    <format>:

      text                     column-aligned textual table
      parseable, parsable      text delimited by vertical bar characters;
                               memory values in MB, times in minutes
      json                     JSON dictionary keyed by partition name;
                               memory values in MB, times in minutes
      yaml                     YAML dictionary keyed by partition name;
                               memory values in MB, times in minutes

    <part-list>:

      one or more partition names separated only by commas (no whitespace)

      e.g.  "part1,part2,part3"

```

The command defaults to displaying in the column-aligned textual table format:

```
$ spart
              FREE    TOTAL     FREE    TOTAL RESOURCE    OTHER   MIN   MAX  MAXJOBTIME   CORES    NODE
PARTITION    CORES    CORES    NODES    NODES  PENDING  PENDING NODES NODES   DAY-HR:MN PERNODE  MEM(GB)
defaultq       448     2436       16       87        0        0     1     -     7-00:00      28     128
shortq         588     2604       21       93        0        0     1     2     0-01:00      28     128
longq           96      336        4       14        0        0     1     -    21-00:00      24      64
bigmemq        252      280        9       10        0        0     1     -     7-00:00      28     512
b224q          560     2548       20       91        0        0     8    40     1-00:00      28     128
core40q        480     1400       12       35      900        0     1     -     7-00:00      40     192
all           1164     4340       37      142        0        0     1     -    NO-LIMIT      24      64
 ```

 The **RESOURCE PENDING** column shows core counts of pending jobs because of the busy resource. The **OTHER PENDING** column shows core counts of pending jobs because of the other reasons such as license or other limits.

 ## Compiling

The CMake build system can be used to drive configuration and compilation of the program:

```
$ mkdir build
$ cd build
$ cmake -DSLURM_PREFIX=/opt/shared/slurm/current \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/opt/shared/slurm/add-ons \
  ..
-- The C compiler identification is GNU 4.8.5
-- Check for working C compiler: /usr/bin/cc
-- Check for working C compiler: /usr/bin/cc -- works
-- Detecting C compiler ABI info
-- Detecting C compiler ABI info - done
-- Found SLURM: /opt/shared/slurm/current/lib/libslurm.so
-- Configuring done
-- Generating done
-- Build files have been written to: /opt/shared/slurm/add-ons/spart/build

$ make install
[100%] Building C object CMakeFiles/spart.dir/spart.c.o
Linking C executable spart
[100%] Built target spart
Install the project...
-- Install configuration: "Release"
-- Installing: /opt/shared/slurm/add-ons/bin/spart
-- Set runtime path of "/opt/shared/slurm/add-ons/bin/spart" to "/opt/shared/slurm/current/lib"
```

## Output formats

The code includes a modular design for outputting the partition summaries.  The original format is presented in the `spart_printer_text` set of functions; it has been altered slightly to size the leading column to meet the maximum partition name length.

The parsable text format (`spart_printer_parsable`) is similar to other Slurm parsable display formats:  values are displayed without whitespace padding with columns delimited by the vertical bar character.  A header row is included.

The JSON and YAML formats (`spart_printer_json` and `spart_printer_yaml`) use key-value dictionaries to organize the information.  The top-level dictionary is keyed by partition name.

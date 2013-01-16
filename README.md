Source2FS
=========

Filesystem that dynamically sources content from multiple backends.

The basic idea is this: you have a "fast" (presumably local) filesystem, and multiple additional filesystems that are
slower, less frequently used, or simply necessarily elsewhere.  Files are available from both the primary and an
arbitrary number of secondary filesystem(s) *simultaneously*.

The use case for which this was developed is a Drupal site with a very large number of very large, rarely-accessed files,
and a relatively small core of files that are accessed frequently.  With source2fs, each webhead carries its own copy of
the fast files, and the slower files are stored remotely on slower (and larger) media.

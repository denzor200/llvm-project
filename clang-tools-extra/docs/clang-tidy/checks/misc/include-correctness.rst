.. title:: clang-tidy - misc-include-correctness

misc-include-correctness
========================

Checks for correctness of angle brackets (``<>``) and quotes (``""``) in
``#include`` statements.

Angle brackets should be used only for system headers (from ``/usr/include``,
STL paths, compiler include paths, etc.), while quotes should be used for
project-local headers.

This check helps enforce the C/C++ convention that system headers use ``<>``
and user headers use ``""``.

Options
-------

.. option:: StrictMode

   When true (default is ``false``), the check uses more aggressive heuristics
   to identify system headers based on file name patterns in addition to
   directory paths.

.. option:: AdditionalSystemIncludes

   A list of additional paths that should be considered as system include
   directories. For example:
   ``['/opt/custom/include', '/third_party/libs']``

Examples
--------

.. code-block:: c++

   // System headers should use angle brackets
   #include <vector>           // Good
   #include "vector"           // Warning

   // User headers should use quotes  
   #include "my_header.h"      // Good
   #include <my_header.h>      // Warning

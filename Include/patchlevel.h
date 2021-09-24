
/* Python version identification scheme.

   When the major or minor version changes, the VERSION variable in
   configure.ac must also be changed.

   There is also (independent) API version information in modsupport.h.
*/

/* Values for PY_RELEASE_LEVEL */
#define PY_RELEASE_LEVEL_ALPHA  0xA
#define PY_RELEASE_LEVEL_BETA   0xB
#define PY_RELEASE_LEVEL_GAMMA  0xC     /* For release candidates */
#define PY_RELEASE_LEVEL_FINAL  0xF     /* Serial should be 0 here */
                                        /* Higher for patch releases */

/* Version parsed out into numeric values */
// If you change the major/minor versions, make sure to change configure.ac as well
/*--start constants--*/
#define PY_MAJOR_VERSION        3
#define PY_MINOR_VERSION        8
#define PY_MICRO_VERSION        12
#define PY_RELEASE_LEVEL        PY_RELEASE_LEVEL_FINAL
#define PY_RELEASE_SERIAL       0

#define PYSTON_MAJOR_VERSION        2
#define PYSTON_MINOR_VERSION        3
#define PYSTON_MICRO_VERSION        1

/* Version as a string */
#define PY_VERSION              "3.8.12"
/*--end constants--*/

/* Version as a single 4-byte hex number, e.g. 0x010502B2 == 1.5.2b2.
   Use this for numeric comparisons, e.g. #if PY_VERSION_HEX >= ... */
#define PY_VERSION_HEX ((PY_MAJOR_VERSION << 24) | \
                        (PY_MINOR_VERSION << 16) | \
                        (PY_MICRO_VERSION <<  8) | \
                        (PY_RELEASE_LEVEL <<  4) | \
                        (PY_RELEASE_SERIAL << 0))


#define PYSTON_VERSION_HEX ((PYSTON_MAJOR_VERSION  << 24) | \
                           (PYSTON_MINOR_VERSION   << 16) | \
                           (PYSTON_MICRO_VERSION   <<  8) | \
                           (0                      <<  4) | \
                           (0                      <<  0))


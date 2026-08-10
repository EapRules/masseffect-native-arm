/*
 * The port's release version — the single place it is written down.
 *
 * A field report is only actionable if it says which build produced it. A log
 * from a release with a GL provider search in it is otherwise byte-identical to
 * a log from the release before it, and there is no way to tell whether the user
 * is running what they just downloaded or an old copy still on the SD card.
 *
 * So the version is defined here and nowhere else:
 *
 *   - the loader prints it as its first trace line, and answers --version;
 *   - the launcher asks the binary (`masseffect --version`) instead of carrying
 *     its own copy of the string, so a stale launcher cannot claim a version
 *     the binary is not;
 *   - the packager reads this header when it reports what it built.
 *
 * Bump it here when cutting a release; nothing else needs editing.
 */
#ifndef MASSEFFECT_PORT_VERSION_H
#define MASSEFFECT_PORT_VERSION_H

#define MASSEFFECT_PORT_VERSION "1.0.5"

#endif /* MASSEFFECT_PORT_VERSION_H */

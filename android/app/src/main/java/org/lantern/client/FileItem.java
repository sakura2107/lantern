package org.lantern.client;

/** Immutable file metadata shared with presentation; contains no transport or system handles. */
class FileItem {
    final String name;
    final boolean directory;
    final long size, modified;
    FileItem(String n,boolean d,long s,long m){name=n;directory=d;size=s;modified=m;}
}

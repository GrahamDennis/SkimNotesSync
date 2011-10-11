Skim Notes Sync
===============

Automatically converts Skim notes saved in extended attributes to .skim files appropriate for Dropbox syncing. Although written to enable syncing Skim notes using Dropbox, it should also work for other sync solutions.

How it works
------------

Skim Notes Sync watches a folder of PDFs (e.g. a BibDesk library of papers) for changes (using FSEvents).  When a PDF document is modified, it is checked for Skim Notes stored in its extended attributes. If they exist, the notes are converted to a .skim file, overwriting one if it existed. The extended attributes are removed from the PDF file as they have now been put in the .skim file.  These .skim files can then be sync'ed normally to other computers.

Skim will normally display a dialog box if a .skim file exists for a PDF which doesn't have any extended attribute notes.  To make Skim automatically import these notes without displaying a dialogue box, use the hidden preference SKReadMissingNotesFromSkimFileOption. Just type:

	defaults write -app Skim SKReadMissingNotesFromSkimFileOption -integer 1

To disable, type:

	defaults delete -app Skim SKReadMissingNotesFromSkimFileOption


Installation
------------

Compile 'Watcher' and copy the executable somewhere useful (~/bin or somewhere in your DropBox folder).

Modify net.grahamdennis.paperswatcher.plist.  The first 'string' entry in 'ProgramArguments' refers to where you have installed 'Watcher'. The second 'string' entry refers to the folder that you want Skim Notes Sync to automatically convert Skim notes in.

Create the directory ~/.PapersWatcher where Skim Notes Sync will store context information. (FIXME: Move this to user preferences.) To do this, type:

	mkdir ~/.PapersWatcher

Copy your modified net.grahamdennis.paperswatcher.plist to ~/Library/LaunchAgents. To start Skim Notes Sync (and have it automatically start on login) type:

	launchctl load ~/Library/LaunchAgents/net.grahamdennis.paperswatcher.plist

To disable Skim Notes Sync, just type:

	launchctl unload ~/Library/LaunchAgents/net.grahamdennis.paperswatcher.plist


Tested on Mac OS X Lion with Dropbox, Skim and a BibDesk library.

Based on the Apple FSEvents Sample Code 'Watcher'.
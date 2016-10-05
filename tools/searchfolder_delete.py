import datetime
import kopano
from MAPI.Tags import PR_CREATION_TIME

for u in kopano.users():
    print 'user', u
    try:
        findroot = u.root.folder('FINDER_ROOT')
    except:
        print 'ERROR getting findroot, skipping user'
        continue
    print findroot, findroot.subfolder_count
    for sf in findroot.folders():
        print sf, sf.entryid, sf.hierarchyid
        creation_time = sf.prop(PR_CREATION_TIME).value
        print 'created at', creation_time
        if creation_time < datetime.datetime.now()-datetime.timedelta(days=30):
            print 'DELETING!'
            findroot.delete(sf)

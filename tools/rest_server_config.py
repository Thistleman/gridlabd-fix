HOST="127.0.0.1" # port IP addr
PORT=5000 # server port
TOKEN = None # Automatically generate access token
DEBUG=False # enable flask debugging
TIMEOUT=3600 # run timeout in seconds
RETENTION=86400 # session retention in seconds
CLEANUP=60 # session cleanup interval in seconds
TMPDIR="/tmp/gridlabd/rest_server" # path to session data
MAXFILE=2**32-1 # maximum file size
MAXLOG=2**20
LOGFILE=TMPDIR+"/server.log"#"/dev/stderr" # logger output
SSL_CONTEXT=None # "adhoc" or ('cert.pem', 'key.pem')

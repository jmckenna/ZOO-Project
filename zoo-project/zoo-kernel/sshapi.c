/*
 *
 * Author : Gérald FENOY
 *
 * Copyright 2017 GeoLabs SARL. All rights reserved.
 *
 * This work was supported by public funds received in the framework of GEOSUD,
 * a project (ANR-10-EQPX-20) of the program "Investissements d'Avenir" managed 
 * by the French National Research Agency
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "sshapi.h"

SSHCON *sessions[MAX_PARALLEL_SSH_CON];

/**
 * Wait until one or more file descriptor has been changed for the socket for a
 * time defined by timeout
 * @param socket_fd int defining the sockket file descriptor
 * @param session an exeisting LIBSSH2_SESSION
 */ 
int waitsocket(int socket_fd, LIBSSH2_SESSION *session)
{
    struct timeval timeout;
    int rc;
    fd_set fd;
    fd_set *writefd = NULL;
    fd_set *readfd = NULL;
    int dir;
 
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
 
    FD_ZERO(&fd);
 
    FD_SET(socket_fd, &fd);
 
    /* now make sure we wait in the correct direction */ 
    dir = libssh2_session_block_directions(session);

 
    if(dir & LIBSSH2_SESSION_BLOCK_INBOUND)
        readfd = &fd;
 
    if(dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
        writefd = &fd;
 
    rc = select(socket_fd + 1, readfd, writefd, NULL, &timeout);
 
    return rc;
}

/**
 * Connect to a remote host using SSH protocol
 * @param conf maps The main configuration maps
 * @return the libssh2 sessions pointer or NULL in case any failure occured. 
 */
SSHCON *ssh_connect(maps* conf){
  unsigned long hostaddr;
  int i, use_pw = 1;
  struct sockaddr_in sin;
  const char *fingerprint;
  SSHCON *result=(SSHCON*)malloc((2*sizeof(int))+sizeof(LIBSSH2_SESSION*)+sizeof(LIBSSH2_SFTP*));
  result->sock_id=NULL;
  result->index=NULL;
  result->session=NULL;
  result->sftp_session=NULL;
  const char *user;
  const char *password=NULL;
  const char *public_key;
  char *private_key;
  int rc;
  FILE *local;
  char mem[1024 * 1000];
  char error[1024];
  int port=22;

  fprintf(stderr,"%s %d\n",__FILE__,__LINE__);
  fflush(stderr);

  map* hpc_host=getMapFromMaps(conf,"HPC","host");
  map* hpc_port=getMapFromMaps(conf,"HPC","port");
  map* hpc_user=getMapFromMaps(conf,"HPC","user");
  map* hpc_password=getMapFromMaps(conf,"HPC","password");
  map* hpc_public_key=getMapFromMaps(conf,"HPC","key");

  fprintf(stderr,"%s %d\n",__FILE__,__LINE__);
  fflush(stderr);

#ifdef WIN32
  WSADATA wsadata;
  int err;
  err = WSAStartup(MAKEWORD(2,0), &wsadata);
  if (err != 0) {
    sprintf(error, "WSAStartup failed with error: %d\n", err);
    setMapInMaps(conf,"lenv","message",error);
    return NULL;
  }
#endif

  // TODO: fetch ip address for the hostname
  if (hpc_host != NULL) {
    hostaddr = inet_addr(hpc_host->value);
  } else {
    setMapInMaps(conf,"lenv","message","No host parameter found in your main.cfg file!\n");
    return NULL;
  }

  // Default port is 22
  if(hpc_port!=NULL){
    port=atoi(hpc_port->value);
  }

  // In case there is no HPC > user the it must failed
  if (hpc_user != NULL) {
    user = hpc_user->value;
  }else{
    setMapInMaps(conf,"lenv","message","No user parameter found in your main.cfg file!");
    return NULL;
  }

  // TODO: in case password is available but there is also the public key
  // defined, then we can consider this password as the pass phrase.
  if (hpc_password != NULL) {
    password = hpc_password->value;
  }else{
    use_pw=-1;
    if (hpc_public_key != NULL) {
      public_key = hpc_public_key->value;
      private_key = strdup(hpc_public_key->value);
      private_key[strlen(public_key)-4]=0;
    }else{
      setMapInMaps(conf,"lenv","message","No method found to authenticate!");
      return NULL;
    }
  }

  rc = libssh2_init (0);
  if (rc != 0) {
    sprintf (error, "libssh2 initialization failed (%d)\n", rc);
    setMapInMaps(conf,"lenv","message",error);
    return NULL;
  }

  result->sock_id = socket(AF_INET, SOCK_STREAM, 0);
  sin.sin_family = AF_INET;
  sin.sin_port = htons(port);
  sin.sin_addr.s_addr = hostaddr;
  if (connect(result->sock_id, (struct sockaddr*)(&sin),sizeof(struct sockaddr_in)) != 0) {
    setMapInMaps(conf,"lenv","message","Failed to connect to remote host!");
    return NULL;
  }

  result->session = libssh2_session_init();
  result->sftp_session = NULL;
  map* tmp=getMapFromMaps(conf,"lenv","nb_sessions");
  if(tmp!=NULL){
    char nb_sessions[10];
    int nb_sess=atoi(tmp->value);
    sprintf(nb_sessions,"%d",nb_sess+1);
    setMapInMaps(conf,"lenv","nb_sessions",nb_sessions);
    result->index=nb_sess+1;
    sessions[nb_sess+1]=result;
  }else{
    setMapInMaps(conf,"lenv","nb_sessions","0");
    sessions[0]=result;
    result->index=0;
  }

  if (!result->session)
    return NULL;

  libssh2_session_set_blocking(result->session, 0);

  while ((rc = libssh2_session_handshake(result->session, result->sock_id))
	 == LIBSSH2_ERROR_EAGAIN);

  if (rc) {
    sprintf(error, "Failure establishing SSH session: %d\n", rc);
    setMapInMaps(conf,"lenv","message",error);
    return NULL;
  }

  fingerprint = libssh2_hostkey_hash(result->session, LIBSSH2_HOSTKEY_HASH_SHA1);
  
  if (use_pw>0) {
    while ((rc = libssh2_userauth_password(result->session, user, password)) ==
	   LIBSSH2_ERROR_EAGAIN);
    if (rc) {
      setMapInMaps(conf,"lenv","message","Authentication by password failed.");
      ssh_close(conf);
      return NULL;
    }
  } else {
    while ((rc = libssh2_userauth_publickey_fromfile(result->session, user,
						     public_key,
						     private_key,
						     password)) ==
	   LIBSSH2_ERROR_EAGAIN);
    if (rc) {
      setMapInMaps(conf,"lenv","message","Authentication by public key failed");
      ssh_close(conf);
      return NULL;
    }
    free(private_key);
  }

  return result;

}

/** 
 * Get the number of opened SSH connections
 * @param conf maps pointer to the main configuration maps
 * @return the number of opened SSH connections
 */
int ssh_get_cnt(maps* conf){
  int result=0;
  map* myMap=getMapFromMaps(conf,"lenv","cnt_session");
  if(myMap!=NULL){
    result=atoi(myMap->value);
  }
  return result;
}

/** 
 * Verify if a file exists on the remote host
 * @param conf maps pointer to the main configuration maps
 * @param targetPath const char* defining the path for storing the file on the
 * remote host
 * @return true in case of success, false if failure occured
 */
size_t ssh_file_exists(maps* conf,const char* targetPath,int cnt){
  size_t result=-1;
  if(cnt>0)
    cnt-=1;
  int rc;
  LIBSSH2_SFTP_ATTRIBUTES attrs;
  LIBSSH2_SFTP_HANDLE *sftp_handle;
  do{
    sftp_handle =
      libssh2_sftp_open(sessions[cnt]->sftp_session, targetPath, LIBSSH2_FXF_READ,
			LIBSSH2_SFTP_S_IRUSR|LIBSSH2_SFTP_S_IWUSR|
			LIBSSH2_SFTP_S_IRGRP|LIBSSH2_SFTP_S_IROTH);
    if (!sftp_handle) {
      fprintf(stderr, "Unable to open file with SFTP: %d %ld\n",__LINE__,
	      libssh2_sftp_last_error(sessions[cnt]->sftp_session));
      return 0;
    }
  }while(!sftp_handle);
  fprintf(stderr, "libssh2_sftp_open() is done, get file information\n");
  do {
  rc = libssh2_sftp_stat_ex(sessions[cnt]->sftp_session, targetPath, strlen(targetPath),
			    LIBSSH2_SFTP_LSTAT, &attrs );
  if (rc<0 &&
      (libssh2_session_last_errno(sessions[cnt]->session) != LIBSSH2_ERROR_EAGAIN))
    {
      fprintf(stderr, "error trying to fstat_ex, returned %d\n", rc);
      break;
    }
  else
    {
      fprintf(stderr, "Stat Data: RetCode=%d\n", rc);
      fprintf(stderr, "Stat Data: Size=%llu\n", attrs.filesize);
      fprintf(stderr, "Stat Data: Perm=%lx\n",  attrs.permissions);
      fprintf(stderr, "Stat Data: mtime=%lu\n",  attrs.mtime);
      if(rc==0)
	break;
      result=attrs.filesize;
    }
  } while (true);
  libssh2_sftp_close(sftp_handle);
  //libssh2_sftp_shutdown(sessions[cnt]->sftp_session);

  return result;
}

/** 
 * Upload a file over an opened SSH connection
 * @param conf maps pointer to the main configuration maps
 * @param localPath const char* defining the local path for accessing the file
 * @param targetPath const char* defining the path for storing the file on the
 * remote host
 * @return true in case of success, false if failure occured
 */
bool ssh_copy(maps* conf,const char* localPath,const char* targetPath,int cnt){
  char mem[1024 * 1000];
  size_t nread;
  size_t memuse=0;
  time_t start;
  long total = 0;
  int duration;
  int rc;
  LIBSSH2_SFTP_HANDLE *sftp_handle;
  //map* myMap=getMapFromMaps(conf,"lenv","cnt_session");
  if(getMapFromMaps(conf,"lenv","cnt_session")!=NULL){
    char tmp[10];
    sprintf(tmp,"%d",cnt+1);
    setMapInMaps(conf,"lenv","cnt_session",tmp);
  }else
    setMapInMaps(conf,"lenv","cnt_session","0");
  FILE *local = fopen(localPath, "rb");
  if (!local) {
    fprintf(stderr, "Can't open local file %s\n", localPath);
    return false;
  }

  do {
    sessions[cnt]->sftp_session = libssh2_sftp_init(sessions[cnt]->session);
    if (!sessions[cnt]->sftp_session &&
	(libssh2_session_last_errno(sessions[cnt]->session) != LIBSSH2_ERROR_EAGAIN)) {
      
      fprintf(stderr, "Unable to init SFTP session\n");
      return false;
    }
  } while (!sessions[cnt]->sftp_session);

  do {
    sftp_handle =
      libssh2_sftp_open(sessions[cnt]->sftp_session, targetPath,
			
			LIBSSH2_FXF_WRITE|LIBSSH2_FXF_CREAT|LIBSSH2_FXF_TRUNC,
			LIBSSH2_SFTP_S_IRUSR|LIBSSH2_SFTP_S_IWUSR|
			LIBSSH2_SFTP_S_IRGRP|LIBSSH2_SFTP_S_IROTH);

    if (!sftp_handle &&
	(libssh2_session_last_errno(sessions[cnt]->session) != LIBSSH2_ERROR_EAGAIN)) {
      
      fprintf(stderr, "Unable to open file with SFTP\n");
      return false;
    }
  } while (!sftp_handle);
  start = time(NULL);
  
  do {
    nread = fread(&mem[memuse], 1, sizeof(mem)-memuse, local);
    if (nread <= 0) {
      if (memuse > 0)
	nread = 0;
      else
	break;
    }
    memuse += nread;
    total += nread;
    
    while ((rc = libssh2_sftp_write(sftp_handle, mem, memuse)) ==
	   LIBSSH2_ERROR_EAGAIN) {
      waitsocket(sessions[cnt]->sock_id, sessions[cnt]->session);
    }
    if(rc < 0)
      break;
    
    if(memuse - rc) {
      memmove(&mem[0], &mem[rc], memuse - rc);
      memuse -= rc;
    }
    else
      memuse = 0;
    
  } while (rc > 0);
  
  duration = (int)(time(NULL)-start);
  fclose(local);
  libssh2_sftp_close_handle(sftp_handle);

  libssh2_sftp_shutdown(sessions[cnt]->sftp_session);
  return true;
}

/** 
 * Download a file over an opened SSH connection
 * @param conf maps pointer to the main configuration maps
 * @param localPath const char* defining the local path for storing the file
 * @param targetPath const char* defining the path for accessing the file on the
 * remote host
 * @return true in case of success, false if failure occured
 */
int ssh_fetch(maps* conf,const char* localPath,const char* targetPath,int cnt){
  char mem[1024];
  size_t nread;
  size_t memuse=0;
  time_t start;
  long total = 0;
  int duration;
  int rc;
  LIBSSH2_SFTP_HANDLE *sftp_handle;
  FILE *local = fopen(localPath, "wb");
  if (!local) {
    fprintf(stderr, "Can't open local file %s\n", localPath);
    return -1;
  }

  do {
    sessions[cnt]->sftp_session = libssh2_sftp_init(sessions[cnt]->session);
    if (!sessions[cnt]->sftp_session &&
	(libssh2_session_last_errno(sessions[cnt]->session) != LIBSSH2_ERROR_EAGAIN)) {
      
      fprintf(stderr, "Unable to init SFTP session\n");
      return false;
    }
  } while (!sessions[cnt]->sftp_session);
    do {
        sftp_handle = libssh2_sftp_open(sessions[cnt]->sftp_session, targetPath,

                                        LIBSSH2_FXF_READ, 0);
 
        if (!sftp_handle) {
            if (libssh2_session_last_errno(sessions[cnt]->session) != LIBSSH2_ERROR_EAGAIN) {
                fprintf(stderr, "Unable to open file with SFTP\n");
                return -1;
            }
            else {
                fprintf(stderr, "non-blocking open\n");
                waitsocket(sessions[cnt]->sock_id, sessions[cnt]->session); 
            }
        }
    } while (!sftp_handle);
 
    int result=1;
    do {
        do {
            rc = libssh2_sftp_read(sftp_handle, mem, sizeof(mem));
            /*fprintf(stderr, "libssh2_sftp_read returned %d\n",
	      rc);*/
            if(rc > 0) {
	      //write(2, mem, rc);
                fwrite(mem, rc, 1, local);
            }
        } while (rc > 0);
 
        if(rc != LIBSSH2_ERROR_EAGAIN) {
	  result=-1;
	  break;
        }

	struct timeval timeout;
	fd_set fd;
        timeout.tv_sec = 10;
        timeout.tv_usec = 0;
 
        FD_ZERO(&fd);
 
        FD_SET(sessions[cnt]->sock_id, &fd);
 
        rc = select(sessions[cnt]->sock_id+1, &fd, &fd, NULL, &timeout);
        if(rc <= 0) {
	  if(rc==0)
            fprintf(stderr, "SFTP download timed out: %d\n", rc);
	  else
	    fprintf(stderr, "SFTP download error: %d\n", rc);
	  result=-1;
	  break;
        }
 
    } while (1);
  duration = (int)(time(NULL)-start);
  fclose(local);
  libssh2_sftp_close_handle(sftp_handle);

  libssh2_sftp_shutdown(sessions[cnt]->sftp_session);
  return 0;
}

/** 
 * Execute a command over an opened SSH connection
 * @param conf maps pointer to the main configuration maps
 * @param command const char pointer to the command to be executed
 * @return bytecount resulting from the execution of the command
 */
int ssh_exec(maps* conf,const char* command,int cnt){
  LIBSSH2_CHANNEL *channel;
  int rc;
  int bytecount = 0;
  int exitcode;
  char *exitsignal=(char *)"none";
  while( (channel = libssh2_channel_open_session(sessions[cnt]->session)) == NULL &&
	 libssh2_session_last_error(sessions[cnt]->session,NULL,NULL,0) == LIBSSH2_ERROR_EAGAIN ) {
      waitsocket(sessions[cnt]->sock_id, sessions[cnt]->session);
  }
  if( channel == NULL ){
    fprintf(stderr,"Error\n");
    return 1;
  }
  while( (rc = libssh2_channel_exec(channel, command)) == LIBSSH2_ERROR_EAGAIN ) {
    waitsocket(sessions[cnt]->sock_id, sessions[cnt]->session);
  }
  if( rc != 0 ) {
    fprintf(stderr,"Error\n");
    return -1;
  }

  map* tmpPath=getMapFromMaps(conf,"main","tmpPath");
  map* uuid=getMapFromMaps(conf,"lenv","usid");
  char *logPath=(char*)malloc((strlen(tmpPath->value)+strlen(uuid->value)+11)*sizeof(char));
  sprintf(logPath,"%s/exec_out_%s",tmpPath->value,uuid->value);
  FILE* logFile=fopen(logPath,"wb");
  while(true){
    int rc;
    do {
      char buffer[0x4000];
      rc = libssh2_channel_read( channel, buffer, sizeof(buffer) );
      
      if( rc > 0 ){
	int i;
	bytecount += rc;
	buffer[rc]=0;
	fprintf(logFile,"%s",buffer);
	fflush(logFile);
      }
    }
    while( rc > 0 );
    
    if( rc == LIBSSH2_ERROR_EAGAIN ) {
      waitsocket(sessions[cnt]->sock_id, sessions[cnt]->session);
    }
    else
      break;
  }
  fclose(logFile);
  exitcode = 127;
  while( (rc = libssh2_channel_close(channel)) == LIBSSH2_ERROR_EAGAIN )
    waitsocket(sessions[cnt]->sock_id, sessions[cnt]->session);
  
  if( rc == 0 ) {
    exitcode = libssh2_channel_get_exit_status( channel );
    libssh2_channel_get_exit_signal(channel, &exitsignal,
				    NULL, NULL, NULL, NULL, NULL);
  }
  
  if (exitsignal)
    fprintf(stderr, "\nGot signal: %s\n", exitsignal);
  else
    fprintf(stderr, "\nEXIT: %d bytecount: %d\n", exitcode, bytecount);
  
  libssh2_channel_free(channel);

  return bytecount;
}

/** 
 * Close an opened SSH connection
 * @param conf maps pointer to the main configuration maps
 * @param con SSHCON pointer to the SSH connection
 * @return true in case of success, false if failure occured
 */
bool ssh_close_session(maps* conf,SSHCON* con){
  while (libssh2_session_disconnect(con->session, "Normal Shutdown, Thank you for using the ZOO-Project sshapi")
	 == LIBSSH2_ERROR_EAGAIN);
#ifdef WIN32
  closesocket(con->sock_id);
#else
  close(con->sock_id);
#endif
  libssh2_session_free(con->session);
  return true;
}

/** 
 * Close all the opened SSH connections
 * @param conf maps pointer to the main configuration maps
 * @return true in case of success, false if failure occured
 */
bool ssh_close(maps* conf){
  int i,nb_sessions;
  map* tmp=getMapFromMaps(conf,"lenv","nb_sessions");
  if(tmp!=NULL){
    nb_sessions=atoi(tmp->value);
    for(i=0;i<nb_sessions;i++)
      ssh_close_session(conf,sessions[i]);
  }
  libssh2_exit();
  return true;
}

bool addToUploadQueue(maps** conf,maps* input){
  map* queueMap=getMapFromMaps(*conf,"uploadQueue","length");
  if(queueMap==NULL){
    maps* queueMaps=createMaps("uploadQueue");
    queueMaps->content=createMap("length","0");
    addMapsToMaps(conf,queueMaps);
    freeMaps(&queueMaps);
    free(queueMaps);
    queueMap=getMapFromMaps(*conf,"uploadQueue","length");
  }
  maps* queueMaps=getMaps(*conf,"uploadQueue");
  int queueIndex=atoi(queueMap->value);
  if(input!=NULL){
    if(getMap(input->content,"cache_file")!=NULL){
      map* length=getMap(input->content,"length");
      if(length==NULL){
	addToMap(input->content,"length","1");
	length=getMap(input->content,"length");
      }
      int len=atoi(length->value);
      int i=0;
      for(i=0;i<len;i++){
	
	map* tmp[2]={getMapArray(input->content,"localPath",i),
		     getMapArray(input->content,"targetPath",i)};

	setMapArray(queueMaps->content,"input",queueIndex,input->name);
	setMapArray(queueMaps->content,"localPath",queueIndex,tmp[0]->value);
	setMapArray(queueMaps->content,"targetPath",queueIndex,tmp[1]->value);
	queueIndex+=1;
      }
    }
  }
#ifdef DEBUG  
  fprintf(stderr,"%s %d\n",__FILE__,__LINE__);
  fflush(stderr);
  dumpMaps(queueMaps);
  fprintf(stderr,"%s %d\n",__FILE__,__LINE__);
  fflush(stderr);
  dumpMaps(*conf);
  fprintf(stderr,"%s %d\n",__FILE__,__LINE__);
  fflush(stderr);
#endif
  return true;
}

bool runUpload(maps** conf){
  SSHCON *test=ssh_connect(*conf);
  if(test==NULL){
    return false;
  }
#ifdef DEBUG
  fprintf(stderr,"%s %d\n",__FILE__,__LINE__);
  fflush(stderr);
  dumpMaps(getMaps(*conf,"uploadQueue"));
  fprintf(stderr,"%s %d\n",__FILE__,__LINE__);
  fflush(stderr);
#endif
  map* queueLengthMap=getMapFromMaps(*conf,"uploadQueue","length");
  maps* queueMaps=getMaps(*conf,"uploadQueue");
  if(queueLengthMap!=NULL){
    int cnt=atoi(queueLengthMap->value);
    int i=0;
    for(i=0;i<cnt;i++){
      map* argv[3]={
	getMapArray(queueMaps->content,"input",i),
	getMapArray(queueMaps->content,"localPath",i),
	getMapArray(queueMaps->content,"targetPath",i)
      };
      fprintf(stderr,"%s %d %s %s\n",__FILE__,__LINE__,argv[1]->value,argv[2]->value);
      ssh_copy(*conf,argv[1]->value,argv[2]->value,ssh_get_cnt(*conf));
    }    
  }
  return true; 
}

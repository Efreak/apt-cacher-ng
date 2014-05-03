#!/usr/bin/perl
use strict;
use Socket;

my %cfg;
my $retcode=0;

$cfg{port} = 3142;
$cfg{adminauth}="";

sub gethostname
{
   return $ENV{"HOSTNAME"} if defined $ENV{"HOSTNAME"};
   eval
   {
      require Sys::Hostname;
      return Sys::Hostname::hostname();
   }
      or do
   {
      #print "ups, $@, using cmd\n";
      my $ret=`hostname`;
      chomp($ret);
      return $ret;
   }
}

sub toBase64
{
   my $src=shift;
   eval
   {
      require MIME::Base64;
      return encode_base64($src);
   }
      or do
   {
      # dirty little helper
      $ENV{"TOBASE64"}=$src;
      return `/usr/sbin/apt-cacher-ng`;
   }
}

for my $file (</etc/apt-cacher-ng/*.conf>)
{
   my $fd;
   open($fd, $file) or next;
   while(<$fd>)
   {
      chomp;
      s/^\s+//;
      s/\s+$//;
      next if /^#/;
			my ($key, $value) = split(/\s*=\s*/);
      if(!defined($value) && /(\w+)\s*:\s*(\S+)/)
      {
         $key=$1;
         $value=$2;
      }
      next unless defined($value);
      #print "$key -> $value\n";
      $cfg{lc($key)}=$value;
   }
}

if($cfg{adminauth})
{
   my $auth="Authorization: Basic ".toBase64($cfg{adminauth});
   chomp($auth);
   $auth.="\r\n";
   $cfg{adminauth}=$auth;
}

my $debug=$ENV{"DEBUG"};

if($ENV{"ACNGIP"})
{
   $cfg{remotehost} = $ENV{"ACNGIP"};
}
elsif($cfg{bindaddress})
{
   $cfg{remotehost}=$cfg{bindaddress};
   $cfg{remotehost}=~s/\s*(\S+)\s*.*/$1/; # only keep the first host
}
else
{
   $cfg{remotehost} = 'localhost';
}

my $SOCK;

sub conTcp
{
   if ($cfg{port} =~ /\D/) { $cfg{port} = getservbyname($cfg{port}, 'tcp') }
   die "No port" unless $cfg{port};
   my $iaddr   = inet_aton($cfg{remotehost})               || return "no such host: $cfg{remotehost}";
   my $paddr   = sockaddr_in($cfg{port}, $iaddr);
   my $proto   = getprotobyname('tcp');
   socket($SOCK, PF_INET, SOCK_STREAM, $proto)  || return "socket: $!\n";
   connect($SOCK, $paddr) || return "connect: $!\n";
   return "";
}

sub conUnix
{
   socket($SOCK, PF_UNIX, SOCK_STREAM,0) || return "socket: $!\n";
   connect($SOCK, sockaddr_un($cfg{socketpath})) || return "connect: $!\n";
   # identify myself
   syswrite $SOCK, "GET / HTTP/1.0\r\nX-Original-Source: localhost\r\n\r\n";
   return "";
}

my $errmsg="";
if($cfg{port})
{
   $errmsg=conTcp;
   if($errmsg)
   {
      die "Cannot connect to server and no alternative (socket file) is available\n$errmsg\n" if(!$cfg{socketpath});
      my $errmsgU=conUnix;
      die "$errmsg\nTrying via socket file... failed:\n$errmsgU\n" if($errmsgU);
   }
}
else
{
   $errmsg=conUnix;
   die "No TCP port specified. Trying via socket file... failed:\n$errmsg\n" if($errmsg);
}

my $acngreq=$ENV{"ACNGREQ"};
if(!$acngreq)
{
   my $aoeflag='&abortOnErrors=aOe'; # checking is default
   $aoeflag='' if(defined $cfg{"exabortonproblems"} and ! $cfg{"exabortonproblems"});
   $acngreq="?doExpire=Start+Expiration$aoeflag";
}

$acngreq="?$acngreq" if ! ($acngreq =~ /^\?/);

syswrite $SOCK, "GET /$cfg{reportpage}$acngreq HTTP/1.0\r\nHost:localhost\r\nConnection:close\r\n$cfg{adminauth}\r\n";
my $line;
while (defined($line = <$SOCK>)) {
   print $line if $debug;
   if($line=~/<!-- TELL:THE:ADMIN -->/)
   #if(1)
   {
      my $HOSTNAME=gethostname;
      
      print STDERR "Error(s) occured while updating volatile index files for apt-cacher-ng.
Please visit http://$HOSTNAME:$cfg{port}/$cfg{reportpage} to rerun the
expiration manually or check the error message(s) in the current log file(s).
\n";
      close STDERR; # stop bothering and just run to end
      $retcode=1;
   }
}

close ($SOCK)            || die "close: $!";
exit $retcode;


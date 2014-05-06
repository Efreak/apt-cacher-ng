#!/usr/bin/perl -w
use strict;
use Socket;
use MIME::Base64;
use Sys::Hostname;

my %cfg;
my $retcode=0;

rescancache:

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

use constant
{
   DO_PROMPT => 0,
   DO_DETAILS => 1,
   DO_REMOVE => 2
};

my $debug=0;

my @killcmd=("rm", "-rf");

@killcmd=("echo", "Killing: ") if $debug;

die "Cannot find CacheDir in the configuration, aborted.\n" if(!$cfg{cachedir});

syswrite(STDOUT, "Scanning $cfg{cachedir}, please wait...\n");
my %distdirs=();
my %archfiles=();

foreach(`find $cfg{cachedir}/. | grep /dists/`)
{
   chomp;
   $_ =~ s!/./!/!g ;
   next if /\/_import/;
   if(/(.*\/dists\/([^\/]+))\//)
   {
      # There may be many of them, spread around different servers!
      # print "jo, $_ ; $1 ; $2 \n" if $debug;
      push(@{$distdirs{$2}}, $1);

      if(/\/(binary|installer)-([^\/]+)\//)
      {
         push(@{$archfiles{$2}}, $_);
      }
   }
}

my $hideidx=0;

my $cmd=0;

sub printPrompt
{
   syswrite(STDOUT, "
WARNING: The removal action may wipe out whole directories containing
         index files. Select d to see detailed list.

(Number nn: tag distribution or architecture nn; 0: exit; d: show details; r: remove tagged; q: quit): ");
}

my @tags;

for(;;)
{
   die "No distribution index files found\n" if( ! keys %distdirs);

   if(DO_PROMPT == $cmd || DO_DETAILS == $cmd)
   {
      syswrite(STDOUT, "Found distributions:\n");
      my $pos=0;
   
      # if there is anything tagged, display only them
      my $taggedcount=0;
      map { $taggedcount++ if $_ } @tags;
      print "bla, taggedcount: $taggedcount\n";

      foreach(keys %distdirs)
      {
         my $tag = ($tags[++$pos] ? "*" : " ");
         print "   $tag ".$pos.". $_ (".scalar@{$distdirs{$_}}." index files)\n";
         if(DO_DETAILS == $cmd && ($tag eq "*" || !$taggedcount))
         {
            my %uno;
            $uno{$_}=1 foreach(@{$distdirs{$_}});
            print "$_\n" foreach(keys %uno);
         }

      }
      syswrite(STDOUT, "\nFound architectures:\n");
      foreach(sort(keys %archfiles))
      {
         my $tag = ($tags[++$pos] ? "*" : " ");
         print "   $tag ".$pos.". $_ (".scalar@{$archfiles{$_}}." index files)\n";
         if(DO_DETAILS == $cmd && ($tag eq "*" || !$taggedcount))
         {
            foreach(@{$archfiles{$_}})
            {
               print "$_\n" unless $_=~/.head$/;
            }
         }
      }
   }
   elsif(DO_REMOVE==$cmd)
   {
      my $pos=0;
      foreach(keys %distdirs)
      {
         if($tags[++$pos])
         {
            my %uno;
            $uno{$_}=1 foreach(@{$distdirs{$_}});
            system(@killcmd, $_) foreach(keys %uno);
         }
      }
      foreach(sort(keys %archfiles))
      {
         if($tags[++$pos])
         {
            system(@killcmd, $_) foreach(@{$archfiles{$_}});
         }
      }
      @tags=();
      syswrite(STDOUT, <<EWARN

NOTE: some package files may become unreferenced now but they will only be
removed after one or multiple expiration runs. To do that immediately, use
the web interface to trigger the expiration or maybe delete unreferenced 
files manually. Press Return to continue...
EWARN
      );
      sysread(STDIN, $pos, 234);

      goto rescancache;
   }

   $cmd=0;
   &printPrompt;

   my $response=0;
   sysread(STDIN, $response, 234);
   exit(0) if($response=~/^q/);

   my $taggedcount=0;
   map { $taggedcount++ if $_ } @tags;

   if($response=~/^d/)
   {
      $cmd=DO_DETAILS;
   }
   elsif($response=~/^\d/)
   {
      foreach my $id (split(/\D/, $response))
      {
         next if($id > 1000);
         #print "got: $id and prev: $tags[$id]" if $debug;
         $tags[$id] = !$tags[$id];
      }
   }
   if($response=~/^r/)
   {
      $cmd=DO_REMOVE if $taggedcount;
   }
   next;
##
##   exit 0 if( ! ($response=~/\d/) || 0 == $response);
##   my $opfer=$tmp[$response];
##   syswrite(STDOUT, "Please wait, removing distribution files of $opfer ...\n");
##   foreach(keys %distdirs)
##   {
##      next if ($distdirs{$_} ne $opfer || !-d $_);
##      print "Removing $_ ...\n";
##      system("/bin/rm", "-r", $_);
##
##      delete $distdirs{$_};
##   }
##   delete $counts{$opfer};
##   syswrite(STDOUT, <<EWARN
##
##NOTE: some package files may become unreferenced now but they will only be
##removed after one or multiple expiration runs. To do that immediately, use
##the web interface to trigger the expiration and maybe delete unreferenced 
##files manually. Press Return to continue...
##EWARN
##);
##   sysread(STDIN, $response, 234);
}


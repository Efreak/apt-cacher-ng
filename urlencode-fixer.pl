#!/usr/bin/perl
use strict;

my $fd;
my $path=shift(@ARGV);
my $dryrun=0;

if ($path eq "-n")
{
   $dryrun=1;
   $path=shift(@ARGV);
}

if (! $path)
{
   die "Usage: $0 [ -n ] directory

Converts some URL encoded filenames to their native version, attempts to
merge directories in the new location if conflicts exist.

WARNING: use with care! Create a backup of your data, at least a tree of
hard links (\"cp -la /var/cache/apt-cacher-ng backupDir\").

Option(s):
   -n     Dry run, only search and report suggested actions
";
}

if(open $fd, "-|", "find", $path, "-type", "f", "-print0")
{
   local $/="\0";
   my @paths=<$fd>;
   for(@paths)
   {
      next if( ! /%(7|5)(b|d)/i);

      s/\0//;
      #print "$_\n";
      my $origpath=$_;
      my @comps=split("/", $_);
      next if(!@comps);

      my $dirnew;
      for(my $i=0; $i<=$#comps; $i++)
      {
         my $part=$comps[$i];
         my $hits=0;

         $hits += $part=~s/%7E/~/g;
         $hits += $part=~s/%5B/]/g;
         $hits += $part=~s/%5D/[/g;

         #print "i: $i, $part\n";
         if($i<$#comps)
         {
            #print "I: is dir\n";
            $dirnew.="/" if defined($dirnew);
            $dirnew.=$part;
            mkdir $dirnew if !$dryrun; # and if it already exists, don't care...
            #print "md: $dirnew\n";
            die "Cannot create target directory: $dirnew\n" if( ! -d $dirnew && ! $dryrun);
         }
         else
         {
            #print "I: is file\n";
            my $tgt="$dirnew/$part";
            print "$origpath -> $tgt\n";
            rename $origpath, $tgt if !$dryrun;
         }
      }
   }
}

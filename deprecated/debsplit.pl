#!/usr/bin/perl
#

use strict;
my %fddset;
my %fdvset;
# temp values within one data set
my @hosts;
my @pathsdeb;
my @pathsvol;

while(<>)
{
   chomp;
   s/^\s+//;
   s/\s+$//;
   next if /^#/;
   if(/(\S+)\s*:\s*(\S+)/)
   {
      my $key=$1;
      my $value=$2;

      if($key eq "Site" || $key eq "Includes")
      {
         push(@hosts, split(/\s/, $value));
      }
      if($key eq "Archive-http")
      {
         push(@pathsdeb, $value);
      }
      if($key eq "Volatile-http")
      {
         push(@pathsvol, $value);
      }
   }
   elsif(@hosts)
   {
      foreach my $h (@hosts)
      {
         $fddset{"$h$_"}=1 foreach @pathsdeb;
         $fdvset{"$h$_"}=1 foreach @pathsvol;
      }
      undef @hosts;
      undef @pathsdeb;
      undef @pathsvol;
   }

}

sub dumpUrls
{
   my $setref=shift;
   my $output=shift;
   my $checkRelease=shift;

   open(fdd, ">$output") || die;

   foreach (sort(keys %$setref))
   {
      my $url="http://$_";
      if($checkRelease)
      {
         $ENV{url}=$url; # let the shell get it right & safe
         if(system("wget -q -t1 -O- --timeout=15 \"\$url/dists/stable/Release\" | grep -q Suite:"))
         {
            print STDERR "Failed mirror URL: $url\n";
            next;
         }
      }
      print fdd "$url\n";
   }
   close(fdd) || die;
   print "$output created\n";
}

dumpUrls(\%fddset, "conf/deb_mirrors", 1);
dumpUrls(\%fdvset, "conf/debvol_mirrors", 0);

# XXX Use Getopt::Long to parse known options, add options for output
# directory, checked/unchecked, with/without Aliases, with/without disabled
# paths

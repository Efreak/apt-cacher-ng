#!/usr/bin/perl


use strict;
my %fddset;

my $arkey=shift(@ARGV);

# temp values within one data set
my @hosts;
my @pathsdeb;

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
      push(@hosts, split(/\s/, $value)) if($key eq "Site" || $key eq "Includes");
      push(@pathsdeb, $value) if($key eq $arkey);
   }
   elsif(@hosts)
   {
      foreach my $h (@hosts)
      {
         s/^/\// if !/\/$/;
         $fddset{"$h$_"}=1 foreach @pathsdeb;
      }
      undef @hosts;
      undef @pathsdeb;
   }
}

print "http://$_\n" foreach (sort(keys %fddset));

exit ! (scalar keys %fddset);

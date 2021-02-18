#!/usr/bin/perl

use strict;

my $sep = '';

while (<>) {
  if (/^CONFIG_NODEMCU_CMODULE_(\S+)=/) {
    print $sep;
    print "-u$1_module_selected1";
    $sep = ' ';
  }
}


#!/usr/bin/perl
use strict;

# checks presence of all files in rules-archive
# just execute and correct

my $filename = "rules-archive";
my $gnuserver = "ftp.gwdg.de/pub/misc/gnu/ftp/gnu";
my $defaultpath = "http://";

my @entries;

print "checking existence of archives in \"$filename\"\n";

open (RULES, "< $filename") or die "Can't open \"$filename\", fix your CDK installation!";

while (<RULES>){
    s/#.*//;
    next if /^(\s)*$/;
    chomp;
    # replace gnuserver
    s/\$\(gnuserver\)/$gnuserver/;
    push @entries, $_;
}
close RULES;

my @errors;
my $errcnt=0;
my $pcnt=1;
foreach (@entries) {
    @_ = split (/;/);
    if (scalar(@_)==1){	# no path, use default
	push @_,$defaultpath;
    }
    
    my $archive = @_[0];
    shift (@_);
    
    print $pcnt++.".: $archive\n";

    foreach (@_){
        print "\t"."$_/$archive";
	my $cmd = "wget --timeout=10 --spider --passive-ftp \"$_/$archive\" 2>&1";
	print " ";
	my @res = `$cmd`;
	if ($?) {
	    push @errors,@res;
	    print "[ERROR]\n";
	    $errcnt++;
	} else {
	    print "[OK]\n";
	}
    }
    print "\n";
}

if (!$errcnt) {
    print "No errors, all archives still reachable!\n\n";
    exit 0;
}

print "\n\nSummary\n-------\n$errcnt error".(($errcnt>1)?"s":"")." occured:\n\n";
foreach(@errors){
    if (m/--/){ print "--------------------------------------------------------------------------------\n"; }
    print;
}


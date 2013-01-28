#!/usr/bin/perl

$project = "docs";
if ($project eq "") {
	$i = rindex($url,'/');
	if ($i < 0) {$i = 0;}
	else {$i++;}
	$project = substr($url,$i);
}

if (!open(S,"svn info --non-interactive |")) { exit 1; }
my $lcrev = "x";
my $i,$lcdate = "unknown";
foreach my $line (<S>) {
	chomp $line;
	my @nv = split(/: +/,$line);
	my $value = $nv[1];
	my $name = $nv[0];

	if ($name =~ m/^Last Changed Rev$/i && $lcrev eq "x") {
		$lcrev = int($value);
	}
	elsif ($name =~ m/^Revision$/i && $lcrev eq "x") {
		$lcrev = int($value);
	}
	elsif ($name =~ m/^Last Changed Date$/i) {
		my @b = split(/ +/,$value);
		my $date = $b[0];
		my $time = $b[1];

		$date =~ s/\-//g;
		$time =~ s/\://g;
		$lcdate = $date."-".$time;
	}
}
close(S);

#my $filename = $project."-rev-".sprintf("%08u",$lcrev)."-src.tar.bz2";
my $filename = "../".$project."-rev-".sprintf("%08u",$lcrev)."-binary.tar";
if (!( -f "$filename.xz" )) {
	print "Packing binary\n";
	print "  to: $filename\n";

	# build the list
	my $list = '',$fn;
	open(XX,"for i in \*.pdf \*.dvi \*.ps \*.txt huedit; do find -iname \$i; done |") || die;
	while ($fn = <XX>) {
		chomp $fn;
		$fn =~ s/^\.\///;
		next unless -f $fn;
		next if $fn =~ m/\.tmp\//;
#		print "$fn\n";
		$list .= "docs/$fn ";
	}
	close(XX);
	die if $list eq '';

	$x = system("tar --exclude=\\\*.obj --exclude=\\\*.lib --exclude=.svn -C .. -cvf $filename $list");
	die unless $x == 0;
	print "Packing to XZ\n";
	$x = system("xz -6e $filename");
	die unless $x == 0;
}


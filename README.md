# GitHub Repo Readme

This repo is a clone of the official debian repo for apt-cacher-ng (I'm going to try and build it on msys2). Currently the official source page is [here](https://tracker.debian.org/pkg/apt-cacher-ng) and git repo is [here](https://salsa.debian.org/blade/apt-cacher-ng.git). The only changes to this repo is to add this README.md file (the original README file is at the bottom of this file if you're too lazy to click it)


Keeping it up to date (we're skipping master, if it changes, we'll need to update it manually)

```
git clone git@github.com:Efreak/apt-cacher-ng
cd apt-cacher-ng
git remote add salsa https://salsa.debian.org/blade/apt-cacher-ng.git
git fetch --all
for branch in $(git ls-remote --heads salsa|egrep -v '^master$'|sed 's#^.*refs/heads/##');
do
  git push origin refs/remotes/salsa/$branch:refs/heads/$branch
done
```

# Official Repo Readme

This is a dummy branch of apt-cacher-ng.

If you are reading this file because you came for the source code, please
checkout upstream/sid or debian/sid or any of the distribution release specific
branches.

In doubt (and having a reference Debian source package), check the file
debian/gbp.conf or the git tags associated with the release of your package.

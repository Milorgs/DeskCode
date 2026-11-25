class Deskcode < Formula
  desc "Native C++20 terminal and desktop AI coding agent"
  homepage "https://github.com/Milorgs/DeskCode"
  version "1.18.4"
  license "MIT"

  on_macos do
    url "https://github.com/Milorgs/DeskCode/releases/download/v#{version}/desk-agent-macos-universal.tar.gz"
    sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  end

  on_linux do
    on_arm do
      url "https://github.com/Milorgs/DeskCode/releases/download/v#{version}/desk-agent-linux-arm64.tar.gz"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
    on_intel do
      url "https://github.com/Milorgs/DeskCode/releases/download/v#{version}/desk-agent-linux-x64.tar.gz"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
  end

  def install
    bin.install "desk"
  end

  test do
    system "#{bin}/desk", "--version"
  end
end
